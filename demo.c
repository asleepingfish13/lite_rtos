/**
 * @file    demo.c
 * @brief   Lite RTOS - Phase 6: 抢占式调度演示
 *
 * Task High  (priority=0, 最高): 每 10 tick 运行一次 (模拟紧急任务)
 * Task Low   (priority=3, 较低): 每 3 tick 运行一次  (模拟后台任务)
 *
 * 抢占行为:
 *   - 当 Task High 延时到期时, 如果 Task Low 正在运行,
 *     tick 信号处理器会检测到高优先级任务就绪，
 *     修改返回地址让 CPU 跳到 schedule() 执行抢占。
 *   - Task High 会立即抢占 Task Low。
 *
 * 在 PC 模拟中, 抢占通过 SIGALRM handler 中修改被中断上下文的
 * 指令指针 (RIP/PC) 实现。硬件上这由 NVIC/PendSV 完成。
 */

#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
#include "task.h"

/* ------------------------------------------------------------------- */
/*  Task High: 高优先级紧急任务                                         */
/* ------------------------------------------------------------------- */
static void task_high_func(void)
{
    int count = 0;
    while (1) {
        printf("  ⚡ [HIGH] tick=%u, count=%d (PREEMPTED!)\n", tick_count, count++);
        if (count >= 6) {
            printf("  ⚡ [HIGH] done.\n");
            exit(0);
        }
        task_delay(10);  /* 每 100ms 运行一次 */
    }
}

/* ------------------------------------------------------------------- */
/*  Task Low: 低优先级后台任务                                          */
/* ------------------------------------------------------------------- */
static void task_low_func(void)
{
    int count = 0;
    while (1) {
        printf("[LOW]  tick=%u, count=%d\n", tick_count, count++);
        if (count >= 12) {
            printf("[LOW]  done.\n");
            exit(0);
        }
        task_delay(3);   /* 每 30ms 运行一次 */
    }
}

/* ------------------------------------------------------------------- */
/*  main                                                                */
/* ------------------------------------------------------------------- */
int main(void)
{
    printf("========================================\n");
    printf("  Lite RTOS - Phase 6 Demo\n");
    printf("  Preemptive Priority Scheduling\n");
    printf("========================================\n");
    printf("  Task High: priority=0, delay=10 ticks\n");
    printf("  Task Low:  priority=3, delay=3 ticks\n");
    printf("  Expect: HIGH preempts LOW when ready\n");
    printf("========================================\n\n");

    kernel_init();

    tcb_t *th = task_create(task_high_func, "high", 0, 0);  /* 优先级 0 = 最高 */
    tcb_t *tl = task_create(task_low_func,  "low",  0, 3);  /* 优先级 3 = 较低 */

    if (th == NULL || tl == NULL) {
        fprintf(stderr, "Failed to create tasks!\n");
        return 1;
    }

    printf("\n=== Starting Preemptive Scheduler ===\n\n");

    kernel_start();

    return 0;
}
