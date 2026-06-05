/**
 * @file    task.c
 * @brief   Lite RTOS - 任务管理实现
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "task.h"
#include "list.h"
#include "port.h"

/* ====================================================================
 *  任务创建
 * ==================================================================== */

tcb_t *task_create(void (*task_func)(void),
                   const char *name,
                   uint32_t stack_size,
                   uint8_t priority)
{
    tcb_t *task;

    /* 参数检查 */
    if (task_func == NULL) {
        fprintf(stderr, "task_create: NULL function pointer\n");
        return NULL;
    }
    if (priority >= MAX_PRIORITIES) {
        fprintf(stderr, "task_create: priority %u out of range (max %u)\n",
                priority, MAX_PRIORITIES - 1);
        return NULL;
    }
    if (g_task_count >= g_max_tasks) {
        fprintf(stderr, "task_create: task count %u reached limit %u\n",
                g_task_count, g_max_tasks);
        return NULL;
    }
    if (stack_size == 0) {
        stack_size = DEFAULT_STACK_SIZE;
    }
    if (stack_size % 16 != 0) {
        stack_size = (stack_size + 15) & ~15u;
    }

    /* 分配 TCB */
    task = (tcb_t *)malloc(sizeof(tcb_t));
    if (task == NULL) {
        fprintf(stderr, "task_create: failed to allocate TCB\n");
        return NULL;
    }

    /* 初始化 TCB */
    memset(task, 0, sizeof(tcb_t));
    task->priority      = priority;
    task->base_priority = priority;
    task->state         = TASK_READY;
    task->stack_size    = stack_size;
    task->delay_ticks   = 0;
    strncpy(task->name, name, sizeof(task->name) - 1);
    task->name[sizeof(task->name) - 1] = '\0';

    /* 分配任务栈 */
    task->stack = (uint8_t *)malloc(stack_size);
    if (task->stack == NULL) {
        fprintf(stderr, "task_create: failed to allocate stack for '%s'\n", name);
        free(task);
        return NULL;
    }

    /* 初始化 ucontext (设置入口函数和栈) */
    port_init_task_context(task, task_func);

    /* 加入就绪列表 */
    task_add_to_ready(task);

    printf("[kernel] Task '%s' created: priority=%u, stack=%u bytes\n",
           task->name, task->priority, task->stack_size);

    g_task_count++;  /* 原子: 仅在主线程调用, 调度器启动前 */

    return task;
}

/* ====================================================================
 *  就绪列表操作
 * ==================================================================== */

void task_add_to_ready(tcb_t *task)
{
    if (task == NULL) return;
    task->state = TASK_READY;
    list_insert_tail(&ready_lists[task->priority], &task->node);
}

void task_remove_from_ready(tcb_t *task)
{
    if (task == NULL) return;
    list_remove(&task->node);
}

/* ====================================================================
 *  任务延时
 *
 *  这是 RTOS 最核心的机制之一:
 *    任务说 "我要等 N 个 tick"，
 *    内核把它从就绪列表移到延时列表，
 *    N 个 tick 后自动唤醒。
 *
 *  FreeRTOS 中叫 vTaskDelay()。
 * ==================================================================== */

void task_delay(uint32_t ticks)
{
    if (ticks == 0) return;

    /* 临界区: 防止 tick 中断修改链表 */
    port_disable_interrupts();

    /* 从就绪列表移除 */
    task_remove_from_ready(current_task);

    /* 设置延时 */
    current_task->state       = TASK_BLOCKED;
    current_task->delay_ticks = ticks;

    /* 加入延时链表尾部
     * (简单实现: 直接加到尾部，在 tick 处理中遍历)
     * 优化: 可以按到期时间排序插入 */
    list_insert_tail(&delayed_list, &current_task->node);

    port_enable_interrupts();

    /* 触发调度: 切换到其他就绪任务 */
    schedule();
}

void task_delay_ms(uint32_t ms)
{
    if (ms == 0) return;

    /* 将毫秒转为 tick 数: ticks = ceil(ms / interval_ms)
     * 内部通过 kernel_get_tick_rate() 反算间隔避免额外依赖。
     * 例如: tick 间隔=10ms → ms=50 → ticks=5
     *       tick 间隔=1ms  → ms=50 → ticks=50 */
    uint32_t hz = kernel_get_tick_rate();
    uint32_t ticks = ((uint64_t)ms * hz + 999) / 1000;
    if (ticks == 0) ticks = 1;  /* 最小延时 1 tick */

    task_delay(ticks);
}
