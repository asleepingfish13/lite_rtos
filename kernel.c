/**
 * @file    kernel.c
 * @brief   Lite RTOS - 内核核心
 *
 * 内核初始化、优先级调度器、tick 处理、任务延时。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        /* 如果有 tick 待处理，让调度器去处理 */
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
 *  schedule - 调度器核心
 *
 *  调用时机:
 *    - task_yield(): 主动让出 CPU
 *    - task_delay(): 延时阻塞
 *    - idle_task:    检测到 tick 待处理
 *    - tick ISR:     抢占 (Phase 6)
 * ------------------------------------------------------------------- */
void schedule(void)
{
    /* 临界区保护 */
    port_disable_interrupts();

    /* 1. 处理待处理的 tick */
    if (port_tick_pending()) {
        process_tick();
    }

    /* 2. 选择下一个任务 */
    tcb_t *next = find_highest_priority_task();
    if (next == NULL) {
        fprintf(stderr, "[scheduler] ERROR: No ready task!\n");
        exit(1);
    }

    /* 3. 如果需要切换，执行上下文切换 */
    if (next != current_task) {
        tcb_t *prev = current_task;

        if (prev != NULL && prev->state == TASK_RUNNING) {
            prev->state = TASK_READY;
        }
        next->state = TASK_RUNNING;

        current_task = next;

        port_enable_interrupts();
        port_switch_context(prev, next);
        /* ---- 任务切换发生在这里 ---- */
        /* 当 prev 再次被调度时，从这里继续 */
    } else {
        port_enable_interrupts();
    }
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
