/**
 * @file    demo_perf.c
 * @brief   Lite RTOS - 性能检测与实时性验证 Demo（演示可视化版）
 *
 * 测试场景: 4 个不同优先级的周期任务，模拟典型实时系统负载。
 * 任务执行时间经过拉长，便于在终端中直观观察抢占和调度流程。
 *
 *   sensor_ctrl  (prio=0, 50ms):  最高优先级传感器控制循环 (~20ms CPU)
 *   watchdog     (prio=1, 300ms): 看门狗心跳 + 触发性能报告 (10 个周期)
 *   matrix_calc  (prio=2, 200ms): CPU 密集型 400×400 矩阵乘法 (~80ms CPU)
 *   data_logger  (prio=4, 200ms): 后台数据记录 (~20ms CPU)
 *
 * 验证目标:
 *   1. 最高优先级任务响应时间有界 (max < 1 tick)
 *   2. CPU 密集型任务被高优先级任务有效抢占 (tick 日志中可见)
 *   3. 上下文切换时间确定性
 *   4. 周期任务抖动可控
 *   5. 无截止期错失
 *
 * 运行时长约 3 秒 (10 × 300ms watchdog 周期)。
 * 观察要点: 终端中 [switch] 行显示 matrix_calc → sensor_ctrl 的抢占模式。
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "kernel.h"
#include "task.h"
#include "perf.h"

/* ====================================================================
 *  计算工作负载 (执行时间根据 modern Mac 调试)
 * ==================================================================== */

/* 轻量计算: sin+cos 循环，约 0.02μs/次 */
static void compute_light(int count)
{
    volatile double r = 0.0;
    for (int i = 0; i < count; i++) {
        r += sin((double)i) * cos((double)i);
    }
    (void)r;
}

/* 中等计算: sqrt 循环，约 0.05μs/次 */
static void compute_medium(int count)
{
    volatile double r = 0.0;
    for (int i = 0; i < count; i++) {
        r += sqrt((double)(i % 10000 + 1));
    }
    (void)r;
}

/*
 * CPU 密集型: 400×400 双精度矩阵乘法（堆分配），循环多次
 *
 *   单次矩阵乘法: 400³ = 64,000,000 次乘加 → 约 30-40ms (modern Mac)
 *   总执行时间 ≈ iterations × 35ms
 *
 *   内存: 3 个 400×400 double 矩阵 ≈ 3 × 1.22MB = 3.66MB (堆分配)
 */
static void compute_heavy(int iterations)
{
    #define MAT_SIZE 400
    const size_t mat_bytes = MAT_SIZE * MAT_SIZE * sizeof(double);

    /* 堆分配: 400×400 太大，栈放不下 */
    double *A = (double *)malloc(mat_bytes);
    double *B = (double *)malloc(mat_bytes);
    double *C = (double *)malloc(mat_bytes);

    if (!A || !B || !C) {
        printf("  ❌ [matrix_calc] ERROR: malloc failed for %dx%d matrices!\n",
               MAT_SIZE, MAT_SIZE);
        free(A); free(B); free(C);
        return;
    }

    for (int iter = 0; iter < iterations; iter++) {
        printf("    ↻ [matrix_calc] iteration %d/%d: computing %dx%d ...\n",
               iter + 1, iterations, MAT_SIZE, MAT_SIZE);

        /* 初始化矩阵 */
        for (int i = 0; i < MAT_SIZE; i++) {
            for (int j = 0; j < MAT_SIZE; j++) {
                A[i * MAT_SIZE + j] = (double)((i * MAT_SIZE + j) % 100) / 100.0;
                B[i * MAT_SIZE + j] = (double)((j * MAT_SIZE + i) % 100) / 100.0;
                C[i * MAT_SIZE + j] = 0.0;
            }
        }

        /* 矩阵乘法: C = A × B  (O(N³) = 400³ = 64M 次乘加) */
        for (int i = 0; i < MAT_SIZE; i++) {
            for (int k = 0; k < MAT_SIZE; k++) {
                double aik = A[i * MAT_SIZE + k];
                for (int j = 0; j < MAT_SIZE; j++) {
                    C[i * MAT_SIZE + j] += aik * B[k * MAT_SIZE + j];
                }
            }
        }

        /* 每轮迭代输出校验和 */
        volatile double checksum = 0.0;
        for (int i = 0; i < MAT_SIZE; i++) {
            checksum += C[i * MAT_SIZE + i];  /* 对角线求和 */
        }
        printf("    ✔ [matrix_calc] iteration %d/%d complete, "
               "diag_checksum = %.6f\n",
               iter + 1, iterations, checksum);
    }

    free(A); free(B); free(C);
    #undef MAT_SIZE
}

/* ====================================================================
 *  任务函数
 * ==================================================================== */

/*
 * sensor_ctrl — 最高优先级传感器控制任务
 *   优先级: 0 (最高)
 *   周期:   50ms
 *   行为:   模拟传感器读取 + 控制律计算 (~20ms CPU)
 *
 *   在终端中观察: 每 5 个 tick 唤醒一次，若 matrix_calc 正在运行则抢占之。
 *   [switch] 行会显示: matrix_calc (prio=2) → sensor_ctrl (prio=0)
 */
static void sensor_ctrl_func(void)
{
    while (1) {
        printf("  ▶ [sensor_ctrl] acquiring data + computing control law...\n");
        compute_light(1000000);  /* ~20ms */
        printf("  ◀ [sensor_ctrl] done, sleeping 50ms\n");

        task_delay_ms(50);
    }
}

/*
 * watchdog — 看门狗 / 测试编排任务
 *   优先级: 1 (次高)
 *   周期:   300ms
 *   行为:   看门狗心跳 (~20ms)，运行 10 次后打印性能报告并退出
 */
static void watchdog_func(void)
{
    for (int cycle = 1; cycle <= 10; cycle++) {
        compute_light(1000000);  /* ~20ms */

        printf("\n┌──────────────────────────────────────────┐\n");
        printf("│ [watchdog] heartbeat #%-2d/10  (tick=%-5u) │\n", cycle, tick_count);
        printf("└──────────────────────────────────────────┘\n\n");

        task_delay_ms(300);
    }

    /* 所有周期完成，打印性能报告 */
    printf("\n");
    printf("[watchdog] Test complete. Generating performance report...\n");

    perf_print_report();

    printf("\n[demo_perf] All tests complete. Exiting.\n");
    exit(0);
}

/*
 * matrix_calc — CPU 密集型矩阵计算任务
 *   优先级: 2 (中等)
 *   周期:   200ms
 *   行为:   400×400 矩阵乘法 × 2 次迭代 (~60-80ms CPU)
 *   内存:   堆分配 3 × 1.22MB ≈ 3.66MB
 *
 *   在终端中观察: 持续运行约 6-8 个 tick，
 *   期间 sensor_ctrl (prio=0) 会多次抢占它。
 *   每次迭代完成时输出校验和。
 *   关键 [switch] 模式:
 *     matrix_calc (prio=2) → sensor_ctrl (prio=0)   ← 抢占!
 *     sensor_ctrl (prio=0) → matrix_calc (prio=2)   ← 恢复
 */
static void matrix_calc_func(void)
{
    while (1) {
        printf("  ⏳ [matrix_calc] starting 400x400 matrix multiply x2...\n");
        compute_heavy(2);  /* ~60-80ms */
        printf("  ✅ [matrix_calc] all iterations complete, sleeping 200ms\n");

        task_delay_ms(200);
    }
}

/*
 * data_logger — 后台数据记录任务
 *   优先级: 4 (低)
 *   周期:   200ms
 *   行为:   模拟日志写入 (~20ms CPU)
 *
 *   仅在 matrix_calc 和 sensor_ctrl 都休眠时才运行。
 *   在终端中观察: 日志行穿插在 sensor_ctrl 和 matrix_calc 之间。
 */
static void data_logger_func(void)
{
    while (1) {
        printf("    📝 [data_logger] writing sensor logs...\n");
        compute_medium(400000);  /* ~20ms */
        printf("    📝 [data_logger] log flush complete, sleeping 200ms\n");

        task_delay_ms(200);
    }
}

/* ====================================================================
 *  主函数
 * ==================================================================== */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   Lite RTOS — Performance & Real-Time Verification  ║\n");
    printf("║            (可视化演示版 - Visual Demo)               ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  Scenario: 4 periodic tasks with mixed priorities\n");
    printf("  ┌──────────────┬──────┬─────────┬──────────────────┐\n");
    printf("  │ Task         │ Prio │ Period  │ CPU Time         │\n");
    printf("  ├──────────────┼──────┼─────────┼──────────────────┤\n");
    printf("  │ sensor_ctrl  │  0   │  50ms   │ ~20ms (快速抢占) │\n");
    printf("  │ watchdog     │  1   │ 300ms   │ ~20ms (编排器)   │\n");
    printf("  │ matrix_calc  │  2   │ 200ms   │ ~80ms (CPU密集) │\n");
    printf("  │ data_logger  │  4   │ 200ms   │ ~20ms  (后台)    │\n");
    printf("  └──────────────┴──────┴─────────┴──────────────────┘\n");
    printf("\n");
    printf("  🔍 观察要点:\n");
    printf("    1. [switch] 行显示 matrix_calc → sensor_ctrl 的抢占\n");
    printf("    2. sensor_ctrl 每 50ms 唤醒并立即抢占 matrix_calc\n");
    printf("    3. matrix_calc 在一次矩阵乘法中被抢占 2-3 次\n");
    printf("    4. data_logger 仅在更高优先级任务都休眠时才运行\n");
    printf("\n");
    printf("  Running for ~3 seconds (10 watchdog cycles)...\n");
    printf("  The performance report will print at the end.\n");
    printf("\n");

    /* 初始化内核 */
    kernel_init();

    /* 设置 tick 间隔为 10ms (100Hz) */
    kernel_set_tick_interval_ms(10);

    /* 创建任务
     *   matrix_calc 使用堆分配 (malloc) 存放 400×400 矩阵，栈正常即可 */
    tcb_t *sensor  = task_create(sensor_ctrl_func,  "sensor_ctrl",  DEFAULT_STACK_SIZE,      0);
    tcb_t *watch   = task_create(watchdog_func,     "watchdog",     DEFAULT_STACK_SIZE,      1);
    tcb_t *matrix  = task_create(matrix_calc_func,  "matrix_calc",  DEFAULT_STACK_SIZE * 2,  2);
    tcb_t *logger  = task_create(data_logger_func,  "data_logger",  DEFAULT_STACK_SIZE,      4);

    if (!sensor || !watch || !matrix || !logger) {
        fprintf(stderr, "[demo_perf] ERROR: Failed to create tasks!\n");
        exit(1);
    }

    /* 设置各任务的目标周期 (用于抖动和截止期检测) */
    perf_task_set_period("sensor_ctrl",   50000000ULL);   /* 50ms */
    perf_task_set_period("watchdog",     300000000ULL);   /* 300ms */
    perf_task_set_period("matrix_calc",  200000000ULL);   /* 200ms */
    perf_task_set_period("data_logger",  200000000ULL);   /* 200ms */

    printf("[demo_perf] All tasks created. Starting scheduler...\n\n");

    /* 启动调度器 (永不返回) */
    kernel_start();

    /* 不可达 */
    return 0;
}
