/**
 * @file    demo_preempt.c
 * @brief   Phase 6: 抢占式优先级调度 (Preemptive Priority Scheduling) 演示
 *
 *   Task High: priority=0 (最高), delay=10 ticks
 *   Task Low:  priority=3,       delay=3  ticks
 *
 * 预期现象:
 *   - LOW 每 3 ticks 运行一次 (短周期)
 *   - HIGH 每 10 ticks 醒来一次，由于优先级更高，立即抢占 LOW
 *   - tick=30 时两者同时到期，HIGH 优先级更高先运行
 */

#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
#include "task.h"

static void task_high_func(void)
{
    int count = 0;
    while (1) {
        printf("  [HIGH] tick=%-4u count=%-2d (PREEMPTED!)\n", tick_count, count++);
        if (count >= 4) {
            printf("\n=== [HIGH] done ===\n");
            exit(0);
        }
        task_delay_ms(100);
    }
}

static void task_low_func(void)
{
    int count = 0;
    while (1) {
        printf("[LOW]   tick=%-4u count=%-2d\n", tick_count, count++);
        if (count >= 12) {
            printf("\n=== [LOW] done ===\n");
            exit(0);
        }
        task_delay_ms(30);
    }
}

int main(void)
{
    printf("========================================\n");
    printf("  Lite RTOS - Phase 6 Demo\n");
    printf("  Preemptive Priority Scheduling\n");
    printf("========================================\n");
    printf("  Task High: priority=0, delay=10 ticks\n");
    printf("  Task Low:  priority=3, delay=3  ticks\n");
    printf("  Expect: HIGH preempts LOW when ready\n");
    printf("========================================\n\n");

    kernel_init();

    /* 设置 tick 间隔: 10ms */
    kernel_set_tick_interval_ms(10);

    task_create(task_high_func, "high", 0,  0);
    task_create(task_low_func,  "low",  0,  3);

    printf("\n=== Starting Preemptive Scheduler ===\n\n");
    kernel_start();

    return 0;
}
