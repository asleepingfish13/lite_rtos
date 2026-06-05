/**
 * @file    demo_tick_rate.c
 * @brief   Lite RTOS - 固定 Tick + 可变间隔演示
 *
 * 展示 kernel_set_tick_interval_ms() + task_delay_ms() 的设计:
 *   - tick 是硬件定时器直设的固定间隔
 *   - task_delay_ms() 始终以真实毫秒为单位，与 tick 间隔无关
 *
 * 场景 A — 环境监测 (tick 间隔 = 500ms)
 *   慢速 tick，省电。task_delay_ms(500) = 1 tick，仍准确 500ms。
 *
 * 场景 B — 小车巡线 (tick 间隔 = 10ms)
 *   快速 tick，高响应。task_delay_ms(50) = 5 ticks，准确 50ms。
 *
 * 关键: task_delay_ms() 在两种场景下语义不变——
 *       传 500 就是等 500ms，不管 tick 是快是慢。
 */

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "kernel.h"
#include "task.h"

/* ====================================================================
 *  Coordinator — 编排两个阶段的演示
 *
 *  Phase 1: 环境监测 (tick 间隔 = 500ms)
 *    task_delay_ms(500) → 内核自动换算为 1 tick
 *
 *  Phase 2: 小车巡线 (tick 间隔 = 10ms)
 *    运行时切换 tick 间隔
 *    task_delay_ms(50) → 内核自动换算为 5 ticks
 * ==================================================================== */
static void coordinator_func(void)
{
    int i;
    uint32_t interval_ms;

    /* ================================================================
     *  Phase 1: 环境监测系统 (tick 间隔 = 500ms)
     * ================================================================ */
    interval_ms = kernel_get_tick_interval_ms();
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  场景 A: 环境监测系统                        ║\n");
    printf("║  tick 间隔 = %ums  (硬件定时器直设)          ║\n", interval_ms);
    printf("║  task_delay_ms(500) → 自动换算为 1 tick      ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    for (i = 1; i <= 6; i++) {
        int temp  = 25 + (int)(tick_count % 5);
        int humid = 60 + (int)(tick_count % 11);

        printf("  [环境监测 tick=%-3u]  🌡温度:%d°C  💧湿度:%d%%  "
               "(真实间隔=%ums)\n",
               tick_count, temp, humid, interval_ms);

        if (i == 6) break;
        task_delay_ms(interval_ms);  /* 等 1 个 tick = 500ms */
    }

    printf("  [环境监测] 采样完成 (共 %u ticks, ~%.1f 秒)\n\n",
           tick_count, tick_count * interval_ms / 1000.0);

    /* ================================================================
     *  Phase 2: 运行时切换 tick 间隔 (500ms → 10ms)
     *
     *  kernel_set_tick_interval_ms() 直写硬件定时器寄存器。
     *  调度器运行中也可以动态调整。
     * ================================================================ */
    kernel_set_tick_interval_ms(10);
    interval_ms = kernel_get_tick_interval_ms();

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  场景 B: 小车巡线系统                        ║\n");
    printf("║  tick 间隔 = %ums  (运行时热切换)            ║\n", interval_ms);
    printf("║  task_delay_ms(50) → 自动换算为 5 ticks      ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    int position = 0;
    for (i = 1; i <= 12; i++) {
        int line_offset = (int)(tick_count % 7) - 3;
        int steer_angle = -line_offset * 5;
        position += 5 + (steer_angle >= 0 ? 1 : -1);

        printf("  [小车巡线 tick=%-3u]  📡红外偏移:%+d  🎯舵机:%+d°  📍位置:%d\n",
               tick_count, line_offset, steer_angle, position);

        task_delay_ms(50);  /* 5 ticks × 10ms = 50ms */
    }

    printf("  [小车巡线] 到达终点! 最终位置: %d (共 %u ticks, ~%.1f 秒)\n\n",
           position, tick_count, tick_count * interval_ms / 1000.0);

    /* ================================================================
     *  总结
     * ================================================================ */
    printf("========================================\n");
    printf("  固定 Tick + 可变间隔 Demo 完成!\n");
    printf("========================================\n");
    printf("  环境监测: interval=%ums → task_delay_ms(500)=1tick\n",
           kernel_get_tick_interval_ms() == 10 ? 500 : 500);
    printf("  小车巡线: interval=%ums → task_delay_ms(50)=5ticks\n",
           kernel_get_tick_interval_ms());
    printf("  task_delay_ms() 与 tick 间隔解耦 ——\n");
    printf("  无论 tick 快慢，参数始终是真实毫秒。\n");
    printf("========================================\n");

    exit(0);
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
    printf("  Lite RTOS - 可变 Tick 间隔 Demo\n");
    printf("========================================\n");
    printf("  固定 tick 硬件 + 可设的毫秒间隔\n");
    printf("  场景 A: %ums/tick — 环境监测 (慢)\n", 500);
    printf("  场景 B: %ums/tick — 小车巡线 (快)\n", 10);
    printf("  task_delay_ms() 始终是真实毫秒!\n");
    printf("========================================\n");

    kernel_init();

    /* Phase 1: 环境监测模式 (500ms/tick = 2Hz) */
    kernel_set_tick_interval_ms(500);

    tcb_t *coord = task_create(coordinator_func, "coordinator", 0, 0);
    if (coord == NULL) {
        fprintf(stderr, "Failed to create coordinator task!\n");
        return 1;
    }

    kernel_start();

    return 0;  /* unreachable */
}
