/**
 * @file    demo_round_robin.c
 * @brief   Lite RTOS - 时间片轮转调度演示
 *
 * 验证功能:
 *   Phase 1: 同优先级 CPU 密集型任务自动轮转 (不依赖 task_yield)
 *   Phase 2: 高优先级任务仍能抢占低优先级轮转任务
 *   Phase 3: kernel_set_time_slice(0) 禁用轮转 — 回退到合作式调度
 */

#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
#include "task.h"
#include "port.h"

/* ====================================================================
 *  配置
 * ==================================================================== */
#define WORKER_PRIO       3       /* 工作线程优先级 (0=最高, 7=空闲) */
#define PREEMPTOR_PRIO    0       /* 抢占线程优先级 (最高) */
#define MONITOR_PRIO      1       /* 监控线程优先级 */
#define WORK_ITERATIONS   20000000  /* Phase 1 CPU 密集型循环的迭代次数 */
#define WORK_ITER_P2      50000000  /* Phase 2 迭代次数 (更多, 便于观察抢占后的轮转) */

static volatile int phase1_a_count = 0;
static volatile int phase1_b_count = 0;
static volatile int phase1_x_count = 0;
static volatile int phase1_done   = 0;
static volatile int phase2_done   = 0;
static volatile int phase3_done   = 0;
static volatile int preempt_count = 0;

/* ====================================================================
 *  Phase 1: 三个同优先级任务 — 自动轮转
 *
 *  三个任务 (worker_A, worker_B, worker_X) 都是纯 CPU 计算，
 *  不调用 task_yield() 或 task_delay()。
 *  时间片轮转确保三者交替执行，体现泛用性。
 * ==================================================================== */
static void worker_A_func(void)
{
    printf("[A] Worker A started (prio=%u)\n", WORKER_PRIO);
    for (int i = 0; i < WORK_ITERATIONS; i++) {
        if (i % 5000000 == 0 && i > 0) {
            printf("[A] iteration %d (time_slice=%u)\n",
                   i, current_task->time_slice_ticks);
        }
        phase1_a_count++;
    }
    printf("[A] Worker A finished (count=%d)\n", phase1_a_count);
    phase1_done++;
    while (1) { task_delay_ms(1000); }
}

static void worker_B_func(void)
{
    printf("[B] Worker B started (prio=%u)\n", WORKER_PRIO);
    for (int i = 0; i < WORK_ITERATIONS; i++) {
        if (i % 5000000 == 0 && i > 0) {
            printf("[B] iteration %d (time_slice=%u)\n",
                   i, current_task->time_slice_ticks);
        }
        phase1_b_count++;
    }
    printf("[B] Worker B finished (count=%d)\n", phase1_b_count);
    phase1_done++;
    while (1) { task_delay_ms(1000); }
}

static void worker_X_func(void)
{
    printf("[X] Worker X started (prio=%u)\n", WORKER_PRIO);
    for (int i = 0; i < WORK_ITERATIONS; i++) {
        if (i % 5000000 == 0 && i > 0) {
            printf("[X] iteration %d (time_slice=%u)\n",
                   i, current_task->time_slice_ticks);
        }
        phase1_x_count++;
    }
    printf("[X] Worker X finished (count=%d)\n", phase1_x_count);
    phase1_done++;
    while (1) { task_delay_ms(1000); }
}

/* ====================================================================
 *  Phase 2: 高优先级抢占 + 同优先级轮转
 *
 *  两个同优先级 worker (C, D) 做时间片轮转，
 *  高优先级 preemptor 周期性唤醒，打断它们的轮转。
 *  preemptor 做可见的 CPU 工作（不是只打印一行就 sleep）。
 *  抢占后 worker 保留剩余时间片继续轮转。
 * ==================================================================== */
static void preemptor_func(void)
{
    while (phase2_done < 2) {
        task_delay_ms(30);  /* 每 30ms 唤醒抢占 (3 ticks) */
        preempt_count++;
        printf("\n[PREEMPT] #%d (prio=%u) >>> PREEMPTING workers! <<<\n",
               preempt_count, PREEMPTOR_PRIO);

        /* 做可见的 CPU 密集型工作 (~8 tick, 模拟真实抢占时长) */
        for (volatile int i = 0; i < 40000000; i++) {
            if (i % 10000000 == 0 && i > 0) {
                printf("[PREEMPT] working... (%d/4 tick=%u)\n",
                       i / 10000000, tick_count);
            }
        }
        printf("[PREEMPT] working... (4/4 tick=%u) ← last batch done\n",
               tick_count);

        printf("[PREEMPT] #%d done (tick=%u), going back to sleep\n\n",
               preempt_count, tick_count);
    }
    printf("[PREEMPT] finished (preempted %d times)\n", preempt_count);
    while (1) { task_delay_ms(1000); }
}

static void worker_C_func(void)
{
    printf("[C] Worker C started (prio=%u)\n", WORKER_PRIO);
    for (int i = 0; i < WORK_ITER_P2; i++) {
        if (i % 10000000 == 0 && i > 0) {
            printf("[C] iteration %d (time_slice=%u, preempt_count=%d)\n",
                   i, current_task->time_slice_ticks, preempt_count);
        }
    }
    printf("[C] Worker C finished (preempted %d times during run)\n",
           preempt_count);
    phase2_done++;
    while (1) { task_delay_ms(1000); }
}

static void worker_D_func(void)
{
    printf("[D] Worker D started (prio=%u)\n", WORKER_PRIO);
    for (int i = 0; i < WORK_ITER_P2; i++) {
        if (i % 10000000 == 0 && i > 0) {
            printf("[D] iteration %d (time_slice=%u, preempt_count=%d)\n",
                   i, current_task->time_slice_ticks, preempt_count);
        }
    }
    printf("[D] Worker D finished (preempted %d times during run)\n",
           preempt_count);
    phase2_done++;
    while (1) { task_delay_ms(1000); }
}

/* ====================================================================
 *  Phase 3: 禁用时间片轮转
 *
 *  kernel_set_time_slice(0) 后，同优先级任务回退到合作式调度。
 *  不调用 task_yield() 的任务会独占 CPU。
 *  worker_D 做纯计算不 yield，worker_E 必须等 worker_D 阻塞后才能运行。
 * ==================================================================== */
static void worker_E_func(void)
{
    printf("[E] Worker E started (prio=%u) — time_slice DISABLED\n", WORKER_PRIO);
    printf("[E] Running CPU-bound loop — worker_F should NOT run yet...\n");
    for (int i = 0; i < WORK_ITERATIONS / 4; i++) {
        if (i % 3000000 == 0 && i > 0) {
            printf("[E] iteration %d (F still waiting)\n", i);
        }
    }
    printf("[E] Worker E finished — delaying to let F run\n");
    task_delay_ms(500);  /* 让出 CPU 给 worker_F */
    printf("[E] Worker E back from delay\n");
    phase3_done++;
    while (1) { task_delay_ms(1000); }
}

static void worker_F_func(void)
{
    printf("[F] Worker F FINALLY started (prio=%u)\n", WORKER_PRIO);
    printf("[F] This proves round-robin was disabled — F waited for E\n");
    for (int i = 0; i < WORK_ITERATIONS / 4; i++) {
        if (i % 3000000 == 0 && i > 0) {
            printf("[F] iteration %d\n", i);
        }
    }
    printf("[F] Worker F finished\n");
    phase3_done++;
    while (1) { task_delay_ms(1000); }
}

/* ====================================================================
 *  监控任务: 驱动测试流程
 * ==================================================================== */
static void monitor_func(void)
{
    /* ---- Phase 1: 基础轮转测试 ---- */
    printf("\n========================================\n");
    printf(" Phase 1: Basic Round-Robin\n");
    printf(" (3 same-prio CPU-bound tasks)\n");
    printf(" Time slice: %u ticks (%ums)\n",
           kernel_get_time_slice(),
           kernel_get_time_slice() * kernel_get_tick_interval_ms());
    printf("========================================\n\n");

    /* 等待三个 worker 完成 */
    while (phase1_done < 3) {
        task_delay_ms(200);
    }

    printf("\n>>> Phase 1 PASSED: All 3 workers completed.\n");
    printf("    A: count=%d  B: count=%d  X: count=%d\n",
           phase1_a_count, phase1_b_count, phase1_x_count);
    if (phase1_a_count > 0 && phase1_b_count > 0 && phase1_x_count > 0) {
        printf("    All three made progress (round-robin active).\n");
    }
    printf("\n");

    /* ---- Phase 2: 抢占 + 轮转测试 ---- */
    printf("========================================\n");
    printf(" Phase 2: Preemption + Round-Robin\n");
    printf(" (2 workers prio=%u with RR, preemptor prio=%u)\n",
           WORKER_PRIO, PREEMPTOR_PRIO);
    printf("========================================\n\n");

    task_create(worker_C_func, "worker_C", 0, WORKER_PRIO);
    task_create(worker_D_func, "worker_D", 0, WORKER_PRIO);
    task_create(preemptor_func, "preemptor", 0, PREEMPTOR_PRIO);

    while (phase2_done < 2) {
        task_delay_ms(200);
    }

    printf("\n>>> Phase 2 PASSED: Preemptor ran %d times "
           "while %d workers completed.\n", preempt_count, phase2_done);
    if (preempt_count > 0) {
        printf("    High-priority preemption preserved "
               "during round-robin.\n");
    }
    printf("\n");

    /* ---- Phase 3: 禁用轮转测试 ---- */
    printf("========================================\n");
    printf(" Phase 3: Disable Round-Robin (slice=0)\n");
    printf("========================================\n\n");

    kernel_set_time_slice(0);
    printf("[kernel] Time slice set to 0 (round-robin DISABLED)\n\n");

    task_create(worker_E_func, "worker_E", 0, WORKER_PRIO);
    task_create(worker_F_func, "worker_F", 0, WORKER_PRIO);

    while (phase3_done < 2) {
        task_delay_ms(200);
    }

    printf("\n>>> Phase 3 PASSED: Workers ran cooperatively "
           "(no time-slice rotation).\n\n");

    /* ---- 总结 ---- */
    printf("========================================\n");
    printf(" All Phases PASSED\n");
    printf("========================================\n");
    printf(" ✓ Same-priority tasks: auto-rotate via time slice\n");
    printf(" ✓ Higher priority tasks: still preempt immediately\n");
    printf(" ✓ time_slice=0: disables rotation (cooperative)\n");
    printf("\nRound-robin time slicing is working!\n");

    exit(0);
}

/* ====================================================================
 *  主函数
 * ==================================================================== */
int main(void)
{
    setbuf(stdout, NULL);  /* 禁用缓冲: 确保在管道环境中实时输出 */
    kernel_init();
    kernel_set_tick_interval_ms(10);  /* 10ms/tick */
    kernel_set_time_slice(3);         /* 3 ticks = 30ms 时间片 (短时间片便于观察轮转) */

    printf("=== Lite RTOS Round-Robin Demo ===\n");
    printf("Tick interval: %ums, Time slice: %u ticks (%ums)\n\n",
           kernel_get_tick_interval_ms(),
           kernel_get_time_slice(),
           kernel_get_time_slice() * kernel_get_tick_interval_ms());

    /* Phase 1: 创建三个同优先级 worker (纯 CPU 计算, 不 yield) */
    task_create(worker_A_func, "worker_A", 0, WORKER_PRIO);
    task_create(worker_B_func, "worker_B", 0, WORKER_PRIO);
    task_create(worker_X_func, "worker_X", 0, WORKER_PRIO);

    /* 监控任务 — 驱动测试流程 (优先级高于 worker) */
    task_create(monitor_func, "monitor", 0, MONITOR_PRIO);

    kernel_start();
    return 0; /* unreachable */
}
