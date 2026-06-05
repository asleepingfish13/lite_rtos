/**
 * @file    demo_task_create.c
 * @brief   Phase 2: 任务控制块 (TCB) 与任务创建演示
 *
 * 创建不同优先级的任务，检查就绪链表结构。
 * 不启动调度器 —— 只展示内核初始化后的静态结构。
 */

#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
#include "task.h"
#include "list.h"

/* 空任务函数 (不会被调度，仅用于创建) */
static void dummy_task_func(void) { while (1) {} }

int main(void)
{
    printf("=== Phase 2: 任务创建 && 就绪链表 Demo ===\n\n");

    kernel_init();

    /* 设置 tick 间隔: 10ms (虽未启动调度器，但保持一致) */
    kernel_set_tick_interval_ms(10);

    /* 在不同优先级上创建任务 */
    task_create(dummy_task_func, "sensor",   0, 1);  /* 优先级 1 */
    task_create(dummy_task_func, "display",  0, 3);  /* 优先级 3 */
    task_create(dummy_task_func, "comm",     0, 3);  /* 优先级 3 (同优先级链表) */
    task_create(dummy_task_func, "storage",  0, 5);  /* 优先级 5 */

    printf("[kernel] 已创建 4 个用户任务 + 1 个 idle 任务\n\n");

    /* 打印每个优先级的就绪链表 */
    printf("=== 就绪链表快照 ===\n");
    for (int p = 0; p < MAX_PRIORITIES; p++) {
        printf("  Priority %d: ", p);
        if (list_is_empty(&ready_lists[p])) {
            printf("(empty)\n");
        } else {
            list_node_t *n;
            list_for_each(n, &ready_lists[p]) {
                tcb_t *t = list_entry(n, tcb_t, node);
                printf("[%s] -> ", t->name);
            }
            printf("sentinel\n");
        }
    }

    printf("\n=== 关键观察 ===\n");
    printf("  1. idle 任务自动创建，钉在最低优先级 (%d)\n", IDLE_PRIORITY);
    printf("  2. 同优先级任务 (display, comm) 都在 Priority 3 的同一个链表里\n");
    printf("  3. 调度器按优先级从 0 到 7 扫描，找到第一个非空链表即返回\n");
    printf("  4. 这种结构保证高优先级任务先被选中\n");

    return 0;
}
