/**
 * @file    demo_preempt_true.c
 * @brief   Phase 7: 真抢占式调度 (True Preemptive Scheduling) 演示
 *
 * 对比 demo_preempt.c (延迟抢占: 只在 task_delay 时切换),
 * 本 demo 验证 ISR 驱动上下文切换 (即时抢占)。
 *
 * 三个任务:
 *   Task PREEMPT:  priority=0 (最高), delay=10 ticks
 *                  每 10 ticks 醒来一次, 应立即抢占 CPU_BURN
 *   Task WATCHDOG: priority=1, delay=30 ticks
 *                  周期性醒来, 可抢占 CPU_BURN
 *                  证明即使有 CPU 密集任务, 中等优先级任务仍能运行
 *   Task CPU_BURN: priority=2 (最低用户优先级), CPU 密集型死循环
 *                  从不调用 task_delay 或 task_yield!
 *
 * 预期现象 (真抢占):
 *   - tick ~10:  PREEMPT 醒来, 立即抢占 CPU_BURN 的死循环
 *   - PREEMPT 打印后调用 task_delay(10), 回到 CPU_BURN
 *   - tick ~20:  PREEMPT 再次抢占
 *   - tick ~30:  PREEMPT 第三次抢占, WATCHDOG 也醒来 (优先级1 > CPU_BURN优先级2)
 *   - PREEMPT task_delay 后, WATCHDOG 抢占 CPU_BURN 运行
 *   - PREEMPT 运行 5 次后 exit(0)
 *
 * 若无真抢占, CPU_BURN 的死循环会永久占用 CPU,
 * PREEMPT 和 WATCHDOG 永远不会运行。
 *
 * 优先级设计:
 *   PREEMPT  (prio=0) > WATCHDOG (prio=1) > CPU_BURN (prio=2)
 * 这样 WATCHDOG 可以抢占 CPU_BURN, 证明低优先级任务不被饿死。
 */

#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
#include "task.h"

/* ====================================================================
 *  Task PREEMPT: 高优先级, 周期性醒来
 * ==================================================================== */
static void task_preempt_func(void)
{
    int count = 0;
    int last_tick = 0;

    while (1) {
        int elapsed = tick_count - last_tick;
        last_tick = tick_count;

        printf("  ⚡ [PREEMPT]  tick=%-5u  elapsed=%d ticks  count=%d  "
               "<<< 我抢占了 CPU! >>>\n",
               tick_count, elapsed, count++);

        if (count >= 8) {
            printf("\n  ✅ 真抢占验证成功! PREEMPT 抢占了 CPU_BURN %d 次\n", count);
            printf("     即使 CPU_BURN 从不调用 task_delay()/task_yield()。\n");
            exit(0);
        }
        task_delay(10);
    }
}

/* ====================================================================
 *  Task CPU_BURN: CPU 密集型死循环 — 永不主动让出 CPU
 *
 *  这是验证真抢占的关键: 如果 PREEMPT 能从信号处理函数中
 *  直接抢占此任务, 则说明真抢占工作正常。
 * ==================================================================== */
static void task_cpu_burn_func(void)
{
    volatile uint64_t counter = 0;
    uint64_t last_reported = 0;

    printf("[CPU_BURN] tick=%-5u  开始 CPU 密集型死循环 "
           "(永不调用 task_delay)\n", tick_count);

    while (1) {
        counter++;  /* 纯 CPU 运算, 不调用任何内核 API */

        /* 每约 50M 次迭代打印一次进度, 证明它一直在运行 */
        if (counter - last_reported > 50000000ULL) {
            last_reported = counter;
            printf("[CPU_BURN] tick=%-5u  仍在运行 "
                   "(counter=%llu, 未被饿死)\n",
                   tick_count, (unsigned long long)counter);
        }
    }
}

/* ====================================================================
 *  Task WATCHDOG: 最低优先级周期性任务
 *
 *  证明在真抢占下, 即使有 CPU 密集任务,
 *  低优先级任务仍能获得 CPU 时间。
 * ==================================================================== */
static void task_watchdog_func(void)
{
    int count = 0;

    while (1) {
        printf("[WATCHDOG] tick=%-5u  我也能运行!  count=%d\n",
               tick_count, count++);

        if (count >= 3) {
            printf("\n  ✅ WATCHDOG 验证成功: "
                   "低优先级任务未被 CPU_BURN 饿死\n");
            exit(0);
        }
        task_delay(30);
    }
}

/* ====================================================================
 *  main
 * ==================================================================== */
int main(void)
{
    printf("========================================\n");
    printf("  Lite RTOS - Phase 7 Demo\n");
    printf("  TRUE Preemptive Scheduling\n");
    printf("========================================\n");
    printf("  PREEMPT:  prio=0  delay=10 ticks  (最高)\n");
    printf("  WATCHDOG: prio=1  delay=30 ticks  (中等)\n");
    printf("  CPU_BURN: prio=2  死循环 (永不 yield, 最低用户优先级)\n");
    printf("\n");
    printf("  关键: CPU_BURN 从不调用 task_delay()\n");
    printf("        或 task_yield(), 它是一个纯 CPU\n");
    printf("        密集型死循环。\n");
    printf("\n");
    printf("  若 PREEMPT 能在 tick≈10 时打印消息,\n");
    printf("  则证明信号处理函数直接完成了抢占调度!\n");
    printf("  WATCHDOG (prio=1) 可抢占 CPU_BURN (prio=2)。\n");
    printf("========================================\n\n");

    kernel_init();

    task_create(task_preempt_func,   "preempt",  0, 0);
    task_create(task_watchdog_func,  "watchdog", 0, 1);
    task_create(task_cpu_burn_func,  "cpu_burn", 0, 2);

    printf("3 个用户任务已创建。\n");
    printf("优先级: PREEMPT(0) > WATCHDOG(1) > CPU_BURN(2)\n");
    printf("注意: CPU_BURN 优先级最低(2), 它启动后将永不主动让出 CPU。\n");
    printf("      若真抢占工作, PREEMPT 将在 10 ticks 后抢占它,\n");
    printf("      WATCHDOG 将在 30 ticks 后抢占 CPU_BURN。\n\n");
    printf("=== 启动真抢占调度器 ===\n\n");

    kernel_start();

    return 0;
}
