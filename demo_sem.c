/**
 * @file    demo_sem.c
 * @brief   Lite RTOS - Phase 11: 计数信号量 Demo (小车传感器场景)
 *
 * ┌──────────────────────────────────────────────────────────────┐
 * │  优先级   任务        角色             同步方式               │
 * ├──────────────────────────────────────────────────────────────┤
 * │  0 (最高) Task A "IR"   红外测距传感器  生产 sem_a_data (×2)  │
 * │  1        Task D "IMU"  惯性测量单元    生产 sem_d_data (×1)  │
 * │  2        Task B "Obst" 障碍物判断      消费 sem_a_data (×1)  │
 * │  3 (最低) Task C "Motor" 电机控制器      消费 sem_a + sem_d    │
 * └──────────────────────────────────────────────────────────────┘
 *
 * 数据流:
 *   Task A ──(ir_distance)──→ [共享变量] ─┬──→ Task B (障碍物分析)
 *                                         └──→ Task C (电机控制)
 *   Task D ──(accel_xyz)────→ [共享变量] ────→ Task C (电机控制)
 *
 * 关键观察点:
 *   1. B/C 在 A 未给信号量前阻塞在 sem_take
 *   2. A give 后 B 先运行 (优先级 2 > 3), C 后运行
 *   3. C 必须同时等待 A 和 D 的数据 (两次 sem_take)
 *   4. D 周期 (70ms) > A 周期 (50ms) → C 有时需等 D
 *   5. ir_distance 逐周期递减模拟小车靠近障碍物
 *   6. 当 ir_distance < 100 时, B 判 DANGER, C 紧急停车 (PWM=0)
 *   7. 每个任务的 multi-tick 工作分步输出, 展示跨 tick 执行
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "kernel.h"
#include "task.h"

/* ====================================================================
 *  共享数据 (模拟传感器读数和控制指令)
 * ==================================================================== */
static volatile int    ir_distance  = 150;   /* 红外距离 (cm), 初始 150 */
static volatile float  accel_x      = 0.0f;  /* X 轴加速度 (m/s²) */
static volatile float  accel_y      = 0.0f;  /* Y 轴加速度 (m/s²) */
static volatile float  accel_z      = 0.0f;  /* Z 轴加速度 (m/s²) */
static volatile int    obstacle     = 0;     /* 障碍物标志: 0=安全, 1=危险 */
static volatile int    motor_left   = 0;     /* 左电机 PWM */
static volatile int    motor_right  = 0;     /* 右电机 PWM */
static volatile int    demo_running = 1;     /* Demo 运行标志 */

/* ====================================================================
 *  信号量 (全局)
 * ==================================================================== */
static sem_t sem_a_data;   /* A 生产数据: IR 距离 */
static sem_t sem_d_data;   /* D 生产数据: IMU 加速度 */

/* 各任务周期计数器 */
static volatile int cycle_a = 0, cycle_d = 0, cycle_b = 0, cycle_c = 0;
#define MAX_CYCLES  3

/* ====================================================================
 *  Task A — 红外测距传感器 (优先级 0, 最高)
 *
 *  模拟传感器工作流程 (3 步, 每步 1 tick):
 *    步骤 1: 传感器预热
 *    步骤 2: ADC 采样
 *    步骤 3: 距离计算
 *
 *  每周期递减 ir_distance, 模拟小车逐渐靠近障碍物。
 *  IR 周期 = 50ms (5 ticks @ 10ms/tick, 不含工作时间)
 * ==================================================================== */
static void task_ir_sensor(void)
{
    while (demo_running && cycle_a < MAX_CYCLES) {
        cycle_a++;

        /* ---- 步骤 1: 传感器预热 (1 tick) ---- */
        printf("[tick %-4u] [IR   ] step 1/3: warming up sensor...\n", tick_count);
        task_delay(1);

        /* ---- 步骤 2: ADC 采样 (1 tick) ---- */
        printf("[tick %-4u] [IR   ] step 2/3: reading ADC...\n", tick_count);
        task_delay(1);

        /* ---- 步骤 3: 距离计算 (1 tick) ---- */
        printf("[tick %-4u] [IR   ] step 3/3: computing distance...\n", tick_count);
        task_delay(1);

        /* 模拟: 小车靠近障碍物, 距离递减 + 随机噪声 */
        ir_distance -= 30;
        if (ir_distance < 10) ir_distance = 10;  /* 最小 10cm */

        printf("[tick %-4u] [IR   ] ★ PRODUCED: ir_distance = %d cm\n",
               tick_count, ir_distance);

        /* 通知两个消费者: B (障碍物分析) 和 C (电机控制) */
        sem_give(&sem_a_data);
        sem_give(&sem_a_data);

        /* 传感器采样周期: 50ms */
        task_delay_ms(50);
    }

    printf("[tick %-4u] [IR   ] ── completed %d cycles, stopping ──\n",
           tick_count, cycle_a);
    while (1) { task_delay(100); }  /* 挂起自己 */
}

/* ====================================================================
 *  Task D — 惯性测量单元 IMU (优先级 1)
 *
 *  模拟 IMU 读取流程 (2 步, 每步 1 tick):
 *    步骤 1: 初始化/唤醒 IMU
 *    步骤 2: 读取加速度寄存器
 *
 *  IMU 周期 = 70ms (7 ticks @ 10ms/tick, 不含工作时间)
 *  故意设得比 IR (50ms) 慢, 让 C 有时需要等待 D。
 * ==================================================================== */
static void task_imu(void)
{
    while (demo_running && cycle_d < MAX_CYCLES) {
        cycle_d++;

        /* ---- 步骤 1: 初始化 IMU (1 tick) ---- */
        printf("[tick %-4u] [IMU  ] step 1/2: initializing IMU...\n", tick_count);
        task_delay(1);

        /* ---- 步骤 2: 读取寄存器 (1 tick) ---- */
        printf("[tick %-4u] [IMU  ] step 2/2: reading accel registers...\n", tick_count);
        task_delay(1);

        /* 模拟加速度计算 (小车运动中) */
        accel_x = 0.5f + (float)(ir_distance - 100) / 200.0f;  /* 前进加速度 */
        accel_y = (float)((rand() % 100) - 50) / 500.0f;       /* 横向抖动 */
        accel_z = 1.0f;                                          /* 重力 */

        printf("[tick %-4u] [IMU  ] ★ PRODUCED: accel=(%+.2f, %+.2f, %+.2f) m/s²\n",
               tick_count, accel_x, accel_y, accel_z);

        /* 通知消费者 C (电机控制) */
        sem_give(&sem_d_data);

        /* IMU 采样周期: 70ms */
        task_delay_ms(70);
    }

    printf("[tick %-4u] [IMU  ] ── completed %d cycles, stopping ──\n",
           tick_count, cycle_d);
    while (1) { task_delay(100); }
}

/* ====================================================================
 *  Task B — 障碍物分析 (优先级 2)
 *
 *  等待 A 的 IR 数据 → 分析 → 判断是否有障碍物。
 *  工作分 2 步 (每步 1 tick):
 *    步骤 1: 阈值分析
 *    步骤 2: 综合判断
 * ==================================================================== */
static void task_obstacle(void)
{
    while (demo_running && cycle_b < MAX_CYCLES) {
        /* ---- 等待 A 的数据 ---- */
        printf("[tick %-4u] [Obst ] waiting for IR data...\n", tick_count);
        sem_take(&sem_a_data);

        int dist = ir_distance;
        printf("[tick %-4u] [Obst ] got ir_distance = %d cm, analyzing...\n",
               tick_count, dist);

        /* ---- 步骤 1: 阈值分析 (1 tick) ---- */
        printf("[tick %-4u] [Obst ] step 1/2: threshold analysis...\n", tick_count);
        task_delay(1);

        /* ---- 步骤 2: 综合判断 (1 tick) ---- */
        printf("[tick %-4u] [Obst ] step 2/2: decision making...\n", tick_count);
        task_delay(1);

        /* 障碍物判断: 距离 < 100cm → 危险 */
        if (dist < 100) {
            obstacle = 1;
            printf("[tick %-4u] [Obst ] ★ RESULT: DANGER! obstacle detected (dist=%d < 100)\n",
                   tick_count, dist);
        } else {
            obstacle = 0;
            printf("[tick %-4u] [Obst ] ★ RESULT: clear, no obstacle (dist=%d >= 100)\n",
                   tick_count, dist);
        }

        cycle_b++;
        task_delay_ms(20);  /* 分析后短暂休息 */
    }

    printf("[tick %-4u] [Obst ] ── completed %d cycles, stopping ──\n",
           tick_count, cycle_b);
    while (1) { task_delay(100); }
}

/* ====================================================================
 *  Task C — 电机控制器 (优先级 3, 最低)
 *
 *  等待 A 的 IR 数据 AND D 的 IMU 数据 → 计算电机 PWM。
 *  工作分 3 步 (每步 1 tick):
 *    步骤 1: 数据融合
 *    步骤 2: PID 计算
 *    步骤 3: PWM 输出
 *
 *  这是唯一需要两个信号量的任务:
 *    先 take sem_a_data (IR),
 *    再 take sem_d_data (IMU).
 *
 *  注意: 如果 D 还没给信号量, C 会在第二个 sem_take 阻塞,
 *        此时 C 已持有 IR 数据但不占用 CPU,
 *        D 给信号量后 C 被唤醒继续计算。
 * ==================================================================== */
static void task_motor(void)
{
    while (demo_running && cycle_c < MAX_CYCLES) {
        /* ---- 等待 A 的 IR 数据 ---- */
        printf("[tick %-4u] [Motor] waiting for IR data...\n", tick_count);
        sem_take(&sem_a_data);

        int dist = ir_distance;
        printf("[tick %-4u] [Motor] got ir_distance = %d cm, now waiting for IMU...\n",
               tick_count, dist);

        /* ---- 等待 D 的 IMU 数据 (可能阻塞!) ---- */
        sem_take(&sem_d_data);

        float ax = accel_x, ay = accel_y;
        printf("[tick %-4u] [Motor] got IMU (ax=%.2f, ay=%.2f), computing motor control...\n",
               tick_count, ax, ay);

        /* ---- 步骤 1: 数据融合 (1 tick) ---- */
        printf("[tick %-4u] [Motor] step 1/3: sensor fusion...\n", tick_count);
        task_delay(1);

        /* ---- 步骤 2: PID 计算 (1 tick) ---- */
        printf("[tick %-4u] [Motor] step 2/3: PID computation...\n", tick_count);
        task_delay(1);

        /* ---- 步骤 3: PWM 输出 (1 tick) ---- */
        printf("[tick %-4u] [Motor] step 3/3: PWM output...\n", tick_count);
        task_delay(1);

        /* 电机控制逻辑:
         *   障碍物检测 → 紧急停车
         *   否则根据距离和加速度调整速度 */
        if (obstacle) {
            motor_left  = 0;
            motor_right = 0;
            printf("[tick %-4u] [Motor] ★ EMERGENCY STOP! obstacle=1, PWM=(%d, %d)\n",
                   tick_count, motor_left, motor_right);
        } else {
            /* 根据 ir_distance 调整速度: 距离越近越慢 */
            int base_speed = (dist > 150) ? 80 : (dist > 100) ? 50 : 25;
            /* 根据横向加速度微调左右轮 */
            motor_left  = base_speed + (int)(ay * 20);
            motor_right = base_speed - (int)(ay * 20);
            printf("[tick %-4u] [Motor] ★ COMMAND: speed=%d, steer=%.1f, PWM=(L:%d, R:%d)\n",
                   tick_count, base_speed, ay * 20, motor_left, motor_right);
        }

        cycle_c++;
        task_delay_ms(30);  /* 控制周期 */
    }

    /* C 是最后一个完成的任务, 终止 demo */
    printf("\n[tick %-4u] [Motor] ── all %d cycles complete ──\n", tick_count, cycle_c);
    printf("[tick %-4u] ========== Demo Finished ==========\n", tick_count);
    demo_running = 0;
    task_delay_ms(50);  /* 给其他任务时间退出循环 */
    exit(0);
}

/* ====================================================================
 *  main — 标准 6 步启动流程
 * ==================================================================== */
int main(void)
{
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  Lite RTOS — Phase 11: 计数信号量 Demo              ║\n");
    printf("║  Car Sensor Scenario: IR + IMU + Obstacle + Motor   ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  优先级 0: Task A \"IR\"    — 红外测距 (生产者)      ║\n");
    printf("║  优先级 1: Task D \"IMU\"   — 惯性测量 (生产者)      ║\n");
    printf("║  优先级 2: Task B \"Obst\"  — 障碍分析 (消费者)      ║\n");
    printf("║  优先级 3: Task C \"Motor\" — 电机控制 (双消费者)    ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  数据依赖: B 需要 A 的数据                           ║\n");
    printf("║            C 需要 A 和 D 的数据                      ║\n");
    printf("║  每任务耗时 ≥ 2 ticks (非单 tick 完成)               ║\n");
    printf("║  观察: sem_take 阻塞 → sem_give 唤醒 → 优先级抢占   ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* 1. 内核初始化 */
    kernel_init();

    /* 2. 设置 tick 间隔: 10ms (便于观察 multi-tick 行为) */
    kernel_set_tick_interval_ms(10);

    /* 3. 初始化信号量 (初始计数值 = 0, 消费者需等待) */
    sem_init(&sem_a_data, 0);
    sem_init(&sem_d_data, 0);
    printf("[main] Semaphores initialized: sem_a_data=0, sem_d_data=0\n\n");

    /* 4. 创建任务 (栈大小传 0 = 使用默认值 8KB) */
    printf("[main] Creating tasks...\n");
    task_create(task_ir_sensor, "IR",     0, 0);  /* prio 0: 最高 */
    task_create(task_imu,       "IMU",    0, 1);  /* prio 1       */
    task_create(task_obstacle,  "Obst",   0, 2);  /* prio 2       */
    task_create(task_motor,     "Motor",  0, 3);  /* prio 3: 最低 */
    printf("[main] 4 tasks created (IR/IMU/Obst/Motor) + 1 idle\n");
    printf("[main] Total user tasks: %u (max: %u)\n\n",
           task_get_count(), kernel_get_max_tasks());

    /* 5. 启动前诊断 */
    printf("[main] Starting scheduler...\n");
    printf("[main] ──────── Demo begins ────────\n\n");

    /* 6. 启动调度器 (永不返回) */
    kernel_start();

    return 0;  /* 不可达 */
}
