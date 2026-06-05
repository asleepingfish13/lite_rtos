/**
 * @file    kernel.c
 * @brief   Lite RTOS - 内核核心
 *
 * 内核初始化、优先级调度器、tick 处理、任务延时。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include "kernel.h"
#include "list.h"
#include "task.h"
#include "port.h"

/* ====================================================================
 *  全局变量定义
 * ==================================================================== */
tcb_t    *current_task;                       /* 当前运行的任务 */
list_t    ready_lists[MAX_PRIORITIES];        /* 就绪链表组 */
list_t    delayed_list;                       /* 延时任务链表 */
tcb_t     idle_task_tcb;                      /* 空闲任务 TCB (静态分配) */
uint32_t  tick_count = 0;                     /* 系统节拍计数 */
uint8_t   scheduler_running = 0;              /* 调度器是否已启动 */

/* 空闲任务栈 */
static uint8_t idle_stack[DEFAULT_STACK_SIZE];

/* ====================================================================
 *  空闲任务
 *
 *  优先级最低，确保 CPU 永不"空闲"。
 *  在真实系统中可以进入低功耗模式 (WFI/WFE)。
 * ==================================================================== */
static void idle_task_func(void)
{
    while (1) {
        /* 安全网轮询: 处理在关中断期间到达的 tick，
         * 以及不支持真抢占的平台 (Windows)。
         * 在真抢占模式 (Unix) 下，schedule_from_isr()
         * 已处理 tick 并清除了 tick_pending，
         * 所以这里通常是快速的 no-op。 */
        if (port_tick_pending()) {
            schedule();
        }
        /* 模拟低功耗等待 (真实系统: __WFI()) */
        port_idle_wait();  /* 1ms, 避免 100% CPU 占用 */
    }
}

/* ====================================================================
 *  Tick 处理
 *
 *  在 schedule() 中调用，处理:
 *    1. 递增系统节拍计数
 *    2. 遍历延时链表，递减 delay_ticks
 *    3. 将到期任务唤醒 (移回就绪列表)
 * ==================================================================== */
static void process_tick(void)
{
    list_node_t *node, *tmp;

    tick_count++;

    /* 每个 tick 打印被中断的任务 (调试用)
     * 注意: 此时调度尚未发生, 显示的是 tick 中断时正在运行的任务。
     * process_tick() 从信号处理函数调用, printf 仅用于 demo 调试。 */
    printf("[tick %-4u]  running: %-10s  (prio=%u)\n",
           tick_count,
           current_task ? current_task->name : "none",
           current_task ? current_task->priority : 0);

    /* 遍历延时链表 (使用 _safe 版本，因为可能修改链表) */
    list_for_each_safe(node, tmp, &delayed_list) {
        tcb_t *task = list_entry(node, tcb_t, node);

        task->delay_ticks--;

        if (task->delay_ticks == 0) {
            /* 延时到期，唤醒任务 */
            list_remove(node);
            task_add_to_ready(task);
        }
    }

    port_clear_tick();
}

/* ====================================================================
 *  调度器
 * ==================================================================== */

/* -------------------------------------------------------------------
 *  find_highest_priority_task - 找到最高优先级的就绪任务
 * ------------------------------------------------------------------- */
static tcb_t *find_highest_priority_task(void)
{
    for (int prio = 0; prio < MAX_PRIORITIES; prio++) {
        if (!list_is_empty(&ready_lists[prio])) {
            list_node_t *node = list_first(&ready_lists[prio]);
            return list_entry(node, tcb_t, node);
        }
    }
    return NULL; /* 不应发生 (idle 任务始终就绪) */
}

/* -------------------------------------------------------------------
 *  schedule_select_and_switch - 调度核心: 选任务 + 切换
 *
 *  查找最高优先级就绪任务，若不同于 current_task 则上下文切换。
 *  该函数同时被 schedule() (任务上下文) 和 schedule_from_isr()
 *  (信号处理函数上下文) 调用。
 *
 *  注意: 为 demo 调试, 此函数中调用了 printf 打印切换日志。
 *  严格意义上 printf 非异步信号安全, 但在 macOS/Linux demo 中可接受。
 * ------------------------------------------------------------------- */
static void schedule_select_and_switch(void)
{
    tcb_t *next = find_highest_priority_task();

    /* idle 任务始终就绪，next 不可能为 NULL */
    if (next == NULL || next == current_task) {
        return;
    }

    tcb_t *prev = current_task;

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    current_task = next;

    /* 上下文切换追踪: 打印谁 → 谁 (调试用) */
    printf("[switch]  tick=%-4u  %-10s (prio=%u)  →  %-10s (prio=%u)\n",
           tick_count,
           prev->name, prev->priority,
           next->name, next->priority);

    port_switch_context(prev, next);
    /* ---- 任务切换发生在这里 ---- */
    /* 当 prev 再次被调度时，从这里继续 */
}

/* -------------------------------------------------------------------
 *  schedule - 调度器核心 (任务上下文入口)
 *
 *  调用时机:
 *    - task_yield(): 主动让出 CPU
 *    - task_delay(): 延时阻塞
 *    - idle_task:    检测到 tick 待处理 (仅 Windows)
 *
 *  Tick 处理策略:
 *    Unix:   由 schedule_from_isr() 在信号上下文中统一处理。
 *            schedule() 不再调用 process_tick()，
 *            避免 tick_count 被 schedule() 和 schedule_from_isr()
 *            重复递增 (同一个物理 tick 被双端处理)。
 *    Windows: schedule() 负责 tick 处理 (无 ISR 驱动)。
 * ------------------------------------------------------------------- */
void schedule(void)
{
    /* 临界区保护 */
    port_disable_interrupts();

#ifdef _WIN32
    /* Windows: tick 由后台线程设置标志，这里处理 */
    if (port_tick_pending()) {
        process_tick();
    }
#else
    /* Unix: tick 统一由 schedule_from_isr() 在信号上下文中处理。
     * 这里仅清除可能残留的 tick_pending 标志（关中断期间遗漏的 tick），
     * 对应的 tick_count 递增和延时递减已在信号处理函数中完成。
     * 不做重复的 process_tick() 调用。 */
    if (port_tick_pending()) {
        port_clear_tick();
    }
#endif

    /* 开中断 → 选任务 → 切换 */
    port_enable_interrupts();
    schedule_select_and_switch();
}

/* -------------------------------------------------------------------
 *  schedule_from_isr - ISR/信号处理函数入口
 *
 *  从 SIGALRM 处理函数 (Unix) 或等效 ISR 上下文调用。
 *  调用者的信号/中断已被硬件/内核屏蔽。
 *
 *  设计要点:
 *    1. 处理 tick (递增 tick_count、唤醒到期任务)
 *    2. 解除 SIGALRM 阻塞，确保切换到的任务可被抢占
 *    3. 选择最高优先级任务并上下文切换
 *    4. 当被抢占任务恢复时，重新阻塞 SIGALRM 匹配信号上下文
 * ------------------------------------------------------------------- */
void schedule_from_isr(void)
{
    /* 1. 处理 tick */
    process_tick();

#ifndef _WIN32
    /* 2. 解除 SIGALRM 阻塞
     *    当前在 SIGALRM 处理函数中，信号被内核自动阻塞。
     *    必须在切换到新任务前解阻塞，否则新任务永远无法被抢占。 */
    {
        sigset_t sigalrm_mask;
        sigemptyset(&sigalrm_mask);
        sigaddset(&sigalrm_mask, SIGALRM);
        sigprocmask(SIG_UNBLOCK, &sigalrm_mask, NULL);
    }
#endif

    /* 3. 调度决策 + 上下文切换 */
    schedule_select_and_switch();
    /* 当被抢占任务恢复时，从此处继续执行
     * 注意: 此后的代码在 swapcontext 之后才执行 —
     * 即被抢占任务下次恢复时, 而非 tick 发生的时刻。 */

#ifndef _WIN32
    /* 4. 恢复信号掩码
     *    重新阻塞 SIGALRM 匹配信号处理函数上下文。
     *    当返回到信号处理函数并 return 后，
     *    sigreturn(2) 会恢复任务原始的 (解阻塞) 掩码。 */
    {
        sigset_t sigalrm_mask;
        sigemptyset(&sigalrm_mask);
        sigaddset(&sigalrm_mask, SIGALRM);
        sigprocmask(SIG_BLOCK, &sigalrm_mask, NULL);
    }
#endif
}

/* ====================================================================
 *  内核 API
 * ==================================================================== */

void kernel_init(void)
{
    printf("[kernel] Lite RTOS Kernel Initializing...\n");

    for (int i = 0; i < MAX_PRIORITIES; i++) {
        list_init(&ready_lists[i]);
    }
    list_init(&delayed_list);

    tick_count = 0;
    scheduler_running = 0;

    /* 创建空闲任务 */
    memset(&idle_task_tcb, 0, sizeof(idle_task_tcb));
    idle_task_tcb.priority      = IDLE_PRIORITY;
    idle_task_tcb.base_priority = IDLE_PRIORITY;
    idle_task_tcb.state         = TASK_READY;
    idle_task_tcb.stack         = idle_stack;
    idle_task_tcb.stack_size    = DEFAULT_STACK_SIZE;
    strncpy(idle_task_tcb.name, "idle", sizeof(idle_task_tcb.name) - 1);

    port_init_task_context(&idle_task_tcb, idle_task_func);
    task_add_to_ready(&idle_task_tcb);

    printf("[kernel] Idle task created (priority=%u)\n", IDLE_PRIORITY);
    printf("[kernel] Kernel initialized.\n");
}

void kernel_start(void)
{
    tcb_t *first;

    printf("[kernel] Starting scheduler...\n");

    /* 启动系统节拍定时器 (Phase 5) */
    port_init_tick_timer();

    scheduler_running = 1;

    first = find_highest_priority_task();
    if (first == NULL) {
        fprintf(stderr, "[kernel] ERROR: No task to start!\n");
        exit(1);
    }

    current_task = first;
    first->state = TASK_RUNNING;

    port_start_first_task(first);
}

void task_yield(void)
{
    port_disable_interrupts();

    task_remove_from_ready(current_task);
    task_add_to_ready(current_task);

    port_enable_interrupts();
    schedule();
}
