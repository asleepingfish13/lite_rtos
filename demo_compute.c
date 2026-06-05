/**
 * @file    demo_compute.c
 * @brief   Phase 8: 真实计算 + 抢占 → 上下文保存/恢复验证
 *
 * 验证目标:
 *   当低优先级任务正在做真实计算（矩阵乘法）时被高优先级任务
 *   （斐波那契数列）抢占，上下文能否正确保存和恢复，
 *   最终计算结果是否正确。
 *
 * 三个任务:
 *   Task FIB:     priority=0 (最高), delay=15 ticks
 *                 迭代计算斐波那契数列, 每次醒来计算下一个数
 *   Task MONITOR: priority=1, delay=25 ticks
 *                 周期性打印存活证明, 证明中等优先级不被饿死
 *   Task MATRIX:  priority=2 (最低)
 *                 N×N 整数矩阵乘法, 逐行计算, 持续循环
 *                 每次完整乘法后验证校验和, 若不一致则报错
 *
 * 优先级设计:
 *   FIB (prio=0) > MONITOR (prio=1) > MATRIX (prio=2)
 *
 * 预期现象:
 *   - MATRIX 正在逐行计算时, 每 15 ticks FIB 醒来抢占
 *   - MATRIX 恢复后从断点继续 (行号连续, 不跳不重)
 *   - 每次完整乘法的校验和与第一次一致 (证明上下文无损坏)
 *   - 每 25 ticks MONITOR 抢占 MATRIX 打印存活
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "kernel.h"
#include "task.h"

/* ====================================================================
 *  矩阵参数
 * ==================================================================== */
#define MATRIX_N        400          /* 400×400 矩阵 */
#define MATRIX_OPS      (MATRIX_N * MATRIX_N * MATRIX_N)  /* 6400 万次乘加 */
#define PROGRESS_EVERY  25           /* 每 25 行打印一次进度 */
#define MATRIX_COMPLETE_TARGET  3    /* 完成 3 次乘法后退出 */

/* ====================================================================
 *  Task MATRIX: 持续 N×N 矩阵乘法 (最低优先级)
 *
 *  在堆上分配矩阵 (避免栈溢出: 3 × 400×400 × 8B ≈ 3.84MB)。
 *  逐行计算 C = A × B, 每 PROGRESS_EVERY 行打印进度。
 *  每次完整乘法后计算校验和并与期望值比对。
 * ==================================================================== */
static void task_matrix_func(void)
{
    /* ---- 堆分配矩阵 (栈上放不下 200×200) ---- */
    size_t mat_bytes = (size_t)MATRIX_N * MATRIX_N * sizeof(int64_t);
    int64_t *A = (int64_t *)malloc(mat_bytes);
    int64_t *B = (int64_t *)malloc(mat_bytes);
    int64_t *C = (int64_t *)malloc(mat_bytes);

    if (A == NULL || B == NULL || C == NULL) {
        fprintf(stderr, "[MATRIX] malloc 矩阵失败!\n");
        exit(1);
    }

    /* ---- 初始化 A 和 B (确定性公式, 每次相同) ---- */
    for (int i = 0; i < MATRIX_N; i++) {
        for (int j = 0; j < MATRIX_N; j++) {
            A[i * MATRIX_N + j] = (int64_t)(i * MATRIX_N + j);
            B[i * MATRIX_N + j] = (int64_t)((i + j) * 2 + 1);
        }
    }

    printf("[MATRIX] tick=%-5u 开始 %dx%d 矩阵乘法 "
           "(约 %d 万次乘加/次, 循环执行)\n",
           tick_count, MATRIX_N, MATRIX_N, MATRIX_OPS / 10000);

    uint64_t expected_checksum = 0;
    int completed = 0;
    int preempt_count = 0;               /* 被抢占次数 */
    uint32_t last_progress_tick = 0;     /* 上次打印进度时的 tick */
    int last_progress_row = 0;           /* 上次打印进度时的行号 */

    while (1) {
        /* ================================================================
         *  C = A × B  逐行计算
         *
         *  这里是抢占发生的关键位置: MATRIX 在内层循环中计算时,
         *  SIGALRM 到达 → schedule_from_isr() → swapcontext()
         *  保存当前 CPU 寄存器 (包括 i,j,k,sum) → 切换到 FIB
         *  FIB 运行完 → swapcontext() 回到这里 → 循环继续
         * ================================================================ */
        for (int i = 0; i < MATRIX_N; i++) {
            for (int j = 0; j < MATRIX_N; j++) {
                int64_t sum = 0;
                for (int k = 0; k < MATRIX_N; k++) {
                    sum += A[i * MATRIX_N + k] * B[k * MATRIX_N + j];
                }
                C[i * MATRIX_N + j] = sum;
            }

            /* 每 PROGRESS_EVERY 行打印一次进度 */
            if ((i + 1) % PROGRESS_EVERY == 0) {
                uint32_t now = tick_count;
                int tick_gap = (last_progress_tick > 0)
                               ? (int)(now - last_progress_tick) : 0;

                if (tick_gap > 1) {
                    /* tick 跳跃 > 3 → 说明被抢占了
                     * 注意: last_progress_row 是上次打印进度时的行号,
                     * 抢占实际发生在 last_progress_row ~ (i+1) 之间 */
                    preempt_count++;
                    printf("[MATRIX] tick=%-5u ⚡ 被抢占后恢复! "
                           "进度断点 row %d → %d "
                           "(间隔 %d ticks, 累计被抢 %d 次)\n",
                           now, last_progress_row, i + 1,
                           tick_gap, preempt_count);
                } else {
                    printf("[MATRIX] tick=%-5u 进度: row %d/%d\n",
                           now, i + 1, MATRIX_N);
                }
                last_progress_tick = now;
                last_progress_row = i + 1;
            }
        }

        /* ---- 完整乘法完成, 计算校验和 ---- */
        uint64_t lo = 0, hi = 0;  /* 128-bit checksum: sum + xor */
        for (int i = 0; i < MATRIX_N * MATRIX_N; i++) {
            uint64_t val = (uint64_t)C[i];
            lo += val;
            hi ^= val;
        }
        uint64_t checksum = lo ^ hi;

        /* ---- 验证 ---- */
        if (expected_checksum == 0) {
            /* 第一次: 记录期望值 */
            expected_checksum = checksum;
            printf("[MATRIX] tick=%-5u ◉ 第 1 次乘法完成! "
                   "校验和=0x%016llx ← 记录为期望值\n",
                   tick_count, (unsigned long long)checksum);
        } else if (checksum == expected_checksum) {
            completed++;
            printf("[MATRIX] tick=%-5u ✅ 第 %d 次乘法验证通过 "
                   "(校验和 0x%016llx 一致)\n",
                   tick_count, completed + 1,
                   (unsigned long long)checksum);

            if (completed >= MATRIX_COMPLETE_TARGET) {
                printf("\n"
                       "  ╔══════════════════════════════════════╗\n"
                       "  ║  ✅ 上下文保存/恢复验证结论         ║\n"
                       "  ╠══════════════════════════════════════╣\n"
                       "  ║  %d 次完整 %d×%d 矩阵乘法               ║\n"
                       "  ║  每次 %d 万次乘加运算                ║\n"
                       "  ║  期间被抢占 %d 次                    ║\n"
                       "  ║  全部校验和一致 → 上下文无损坏       ║\n"
                       "  ╚══════════════════════════════════════╝\n\n",
                       completed + 1, MATRIX_N, MATRIX_N,
                       MATRIX_OPS / 10000, preempt_count);
                free(A); free(B); free(C);
                exit(0);
            }
        } else {
            printf("\n"
                   "  ╔══════════════════════════════════════╗\n"
                   "  ║  ❌ 校验和错误!                      ║\n"
                   "  ║  期望: 0x%016llx  ║\n"
                   "  ║  实际: 0x%016llx  ║\n"
                   "  ║  上下文保存/恢复有 BUG!              ║\n"
                   "  ╚══════════════════════════════════════╝\n\n",
                   (unsigned long long)expected_checksum,
                   (unsigned long long)checksum);
            free(A); free(B); free(C);
            exit(1);
        }
    }
}

/* ====================================================================
 *  Task FIB: 批量计算斐波那契数列 (最高优先级)
 *
 *  每 20 ticks 醒来, 批量计算 80 万个斐波那契数。
 *  批量计算耗时约 3~5 个 tick, 能在 per-tick 日志中体现。
 *  前几个数用 Binet 公式验证正确性。
 * ==================================================================== */

#define FIB_BATCH_SIZE  10000000 /* 每次醒来计算 1000 万个数 (~3 ticks) */
#define FIB_BATCHES     10       /* 计算 10 个批次后停止 */
#define FIB_DELAY_MS    60        /* 批次间隔 60ms */

static void task_fib_func(void)
{
    uint64_t a = 0, b = 1;   /* F(0)=0, F(1)=1 */
    int total_batches = 0;
    uint64_t total_count = 0;

    /* Binet 公式常量 */
    const double phi   = 1.6180339887498948482;
    const double sqrt5 = 2.2360679774997896964;

    printf("[FIB]    tick=%-5u 开始批量计算斐波那契数列 "
           "(每 %u ms 醒来, 批量 %d 个)\n",
           tick_count, FIB_DELAY_MS, FIB_BATCH_SIZE);

    while (1) {
        uint32_t start_tick = tick_count;

        /* ---- 批量计算 FIB_BATCH_SIZE 个斐波那契数 ---- */
        volatile uint64_t dummy = 0;  /* 防止编译器过度优化 */
        for (int i = 0; i < FIB_BATCH_SIZE; i++) {
            uint64_t next = a + b;   /* 允许自然溢出 (模 2^64) */
            a = b;
            b = next;
            /* 加一个哈希扰动, 确保循环体足够重 (跨 tick) */
            dummy ^= next;
            dummy = (dummy * 0x5851F42D4C957F2DULL) + 0x14057B7EF767814FULL;
        }
        (void)dummy;
        total_count += FIB_BATCH_SIZE;

        uint32_t elapsed = tick_count - start_tick;
        total_batches++;

        /* ---- 抽查验证前几个数 (Binet 公式) ---- */
        /* 注意: a, b 现在是第 (total_count-1) 和第 total_count 个数,
         *       以 F(0)=0, F(1)=1 为起始 */
        uint64_t check_val = b;
        char *ok = "";
        if (total_count <= 70) {
            uint64_t predicted = (uint64_t)(pow(phi, total_count + 1) / sqrt5 + 0.5);
            ok = (check_val == predicted) ? " OK" : " FAIL";
        }

        printf("[FIB]    tick=%-5u 批次#%d: 计算 %d 个数 → F(%llu) 末值=%llu"
               "  耗时 %u ticks%s\n",
               tick_count, total_batches, FIB_BATCH_SIZE,
               (unsigned long long)(total_count + 1),
               (unsigned long long)check_val,
               elapsed, ok);

        if (total_batches >= FIB_BATCHES) {
            printf("\n"
                   "  ╔══════════════════════════════════════╗\n"
                   "  ║  ✅ 斐波那契完成                      ║\n"
                   "  ║  %d 批次, 共 %llu 个数               ║\n"
                   "  ╚══════════════════════════════════════╝\n\n",
                   total_batches,
                   (unsigned long long)total_count);
            /* 不再运行, 让 MATRIX 完成剩余工作 */
            while (1) task_delay_ms(99990);
        }

        task_delay_ms(FIB_DELAY_MS);
    }
}

/* ====================================================================
 *  Task MONITOR: 系统存活证明 (中等优先级)
 *
 *  每 25 ticks 打印一次, 证明在 MATRIX 死循环下
 *  中等优先级任务仍能获得 CPU。
 * ==================================================================== */
static void task_monitor_func(void)
{
    int count = 0;

    while (1) {
        printf("[MONITOR] tick=%-5u 系统存活! (第 %d 次检查)\n",
               tick_count, ++count);
        task_delay_ms(250);
    }
}

/* ====================================================================
 *  main
 * ==================================================================== */
int main(void)
{
    printf("========================================\n");
    printf("  Lite RTOS - Phase 8 Demo\n");
    printf("  真实计算 + 抢占 → 上下文验证\n");
    printf("========================================\n");
    printf("  FIB:     prio=0  斐波那契 每 %ums 醒来\n",
           FIB_DELAY_MS);
    printf("  MONITOR: prio=1  系统存活 (每 250ms)\n");
    printf("  MATRIX:  prio=2  %dx%d 矩阵乘法 (持续)\n",
           MATRIX_N, MATRIX_N);
    printf("\n");
    printf("  验证: 矩阵乘法中途被抢占后,\n");
    printf("        恢复执行结果是否正确。\n");
    printf("========================================\n\n");

    kernel_init();

    /* 设置 tick 间隔: 10ms */
    kernel_set_tick_interval_ms(10);

    task_create(task_fib_func,     "fib",     0,      0);
    task_create(task_monitor_func, "monitor", 0,      1);
    task_create(task_matrix_func,  "matrix",  131072, 2);  /* 128KB 栈 */

    printf("3 个计算任务已创建。\n");
    printf("MATRIX 栈=128KB (矩阵在堆上分配, 约 3.84MB)\n\n");
    printf("=== 启动真抢占调度器 ===\n\n");

    kernel_start();

    return 0;
}
