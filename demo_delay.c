/**
 * @file    demo.c
 * @brief   Lite RTOS - 延时链表 (Delayed List) 演示
 *
 * 三个任务，三个不同的延时长度。展示任务如何在 ready_list 和
 * delayed_list 之间来回切换。
 *
 *   Task Fast:   delay=2 ticks  (20ms)  — 快节奏，频繁跑
 *   Task Medium: delay=5 ticks  (50ms)  — 中等节奏
 *   Task Slow:   delay=8 ticks  (80ms)  — 慢节奏
 *
 * 预期现象:
 *   - 三个任务同时启动 (tick=0)，然后各自进入延时链表
 *   - Fast 最先到期 (tick=2)，从延时链表回到就绪链表
 *   - 因为三者优先级相同，到期后按 FIFO 轮流跑
 *   - 每次任务打印时附带延时链表状态
 */

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "kernel.h"
#include "task.h"
#include "list.h"

/* ====================================================================
 *  辅助函数：打印延时链表中的所有任务
 * ==================================================================== */
static void dump_delayed_list(void)
{
    printf("    [delayed_list]: ");
    if (list_is_empty(&delayed_list)) {
        printf("(empty)");
    } else {
        list_node_t *n;
        list_for_each(n, &delayed_list) {
            tcb_t *t = list_entry(n, tcb_t, node);
            printf("[%s:%-u] ", t->name, t->delay_ticks);
        }
    }
    printf("\n");
}

/* ====================================================================
 *  打印就绪链表
 * ==================================================================== */
static void dump_ready_lists(void)
{
    printf("    [ready_lists]: ");
    int found = 0;
    for (int p = 0; p < MAX_PRIORITIES; p++) {
        list_node_t *n;
        list_for_each(n, &ready_lists[p]) {
            tcb_t *t = list_entry(n, tcb_t, node);
            printf("[%s@prio%d] ", t->name, t->priority);
            found = 1;
        }
    }
    if (!found) printf("(empty)");
    printf("\n");
}

/* ====================================================================
 *  Task Fast — 每 2 ticks 跑一次
 * ==================================================================== */
static void task_fast_func(void)
{
    int count = 0;
    while (1) {
        printf("\n[FAST]  tick=%-4u count=%-2d  <<< 我醒了！\n", tick_count, count++);
        dump_ready_lists();
        dump_delayed_list();

        if (count >= 10) {
            printf("\n=== [FAST] done, exiting ===\n");
            exit(0);
        }

        printf("[FAST]  → task_delay(2), 进入延时链表...\n");
        task_delay(2);
    }
}

/* ====================================================================
 *  Task Medium — 每 5 ticks 跑一次
 * ==================================================================== */
static void task_medium_func(void)
{
    int count = 0;
    while (1) {
        printf("\n[MED]   tick=%-4u count=%-2d  <<< 我醒了！\n", tick_count, count++);
        dump_ready_lists();
        dump_delayed_list();

        if (count >= 8) {
            printf("\n=== [MED] done, exiting ===\n");
            exit(0);
        }

        printf("[MED]   → task_delay(5), 进入延时链表...\n");
        task_delay(5);
    }
}

/* ====================================================================
 *  Task Slow — 每 8 ticks 跑一次
 * ==================================================================== */
static void task_slow_func(void)
{
    int count = 0;
    while (1) {
        printf("\n[SLOW]  tick=%-4u count=%-2d  <<< 我醒了！\n", tick_count, count++);
        dump_ready_lists();
        dump_delayed_list();

        if (count >= 6) {
            printf("\n=== [SLOW] done, exiting ===\n");
            exit(0);
        }

        printf("[SLOW]  → task_delay(8), 进入延时链表...\n");
        task_delay(8);
    }
}

/* ====================================================================
 *  main
 * ==================================================================== */
int main(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    printf("========================================\n");
    printf("  Lite RTOS - Delayed List Demo\n");
    printf("========================================\n");
    printf("  Task Fast:   delay=2 ticks (20ms)\n");
    printf("  Task Medium: delay=5 ticks (50ms)\n");
    printf("  Task Slow:   delay=8 ticks (80ms)\n");
    printf("  All priority=1 (same, FIFO order)\n");
    printf("========================================\n\n");

    kernel_init();

    tcb_t *tf = task_create(task_fast_func,   "fast",   0, 1);
    tcb_t *tm = task_create(task_medium_func, "medium", 0, 1);
    tcb_t *ts = task_create(task_slow_func,   "slow",   0, 1);

    if (tf == NULL || tm == NULL || ts == NULL) {
        fprintf(stderr, "Failed to create tasks!\n");
        return 1;
    }

    printf("=== t=0: 三个任务都在就绪链表 ===\n");
    dump_ready_lists();
    dump_delayed_list();

    printf("\n=== 启动调度器 ===\n");
    kernel_start();

    return 0;
}
