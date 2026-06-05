/**
 * @file    demo_car.c
 * @brief   综合 Demo: 模拟小车巡线运行 + 到达目的地拍照
 *
 * 场景: 智能小车沿黑线行驶，实时采集多传感器数据，
 *       到达目的地后触发摄像头拍照，展示多任务协作。
 *
 * 任务列表 (6 个用户任务 + idle):
 *   IR 传感器   prio=0  cycle=20ms  最高优先级，实时检测障碍物
 *   巡线控制    prio=1  cycle=40ms  核心: 读取线偏移，输出转向修正
 *   速度传感器  prio=2  cycle=60ms  定期读取车轮编码器
 *   加速度计    prio=3  cycle=100ms 读取 IMU 三轴加速度
 *   遥测显示    prio=4  cycle=200ms 汇总打印所有传感器数据
 *   摄像头      prio=5  one-shot    到达目的地后拍照 (信号量触发)
 *   Idle        prio=7              CPU 空闲时运行
 */

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "kernel.h"
#include "task.h"

/* ====================================================================
 *  模拟硬件状态 (共享数据，被各个"传感器任务"更新)
 * ==================================================================== */
static int car_position    = 0;    /* 当前位置 0~100 */
static int line_offset     = 0;    /* 偏离黑线的偏移量 (mm)，正=偏右，负=偏左 */
static int car_speed       = 30;   /* 当前速度 cm/s */
static float accel_x       = 0.0f; /* IMU X 轴加速度 (g) */
static float accel_y       = 0.0f; /* IMU Y 轴加速度 (g) */
static float accel_z       = 1.0f; /* IMU Z 轴加速度 (g) */
static int ir_distance     = 150;  /* 红外测距: 前方障碍物距离 (cm) */
static int destination     = 0;    /* 是否到达目的地 (0=否, 1=是) */
static int photo_serial    = 0;    /* 照片序号 */

/* ====================================================================
 *  Task 1: 红外距离传感器 (最高优先级，保证安全)
 * ==================================================================== */
static void ir_sensor_task(void)
{
    int count = 0;
    while (!destination) {
        count++;
        /* 模拟: 红外测距值在 50~200cm 间波动 */
        ir_distance = 150 + (int)(tick_count % 13) * 3 - 18
                      + (count % 7) * 2;
        /* 模拟: 根据位置计算线偏移 (位置越大，越接近终点，线越直) */
        line_offset = (int)((100 - car_position) * 0.3f) % 10 - 5
                      + (count % 3);

        printf("  [IR 传感器]  tick=%-3u  前方=%dcm  线偏移=%+dmm\n",
               tick_count, ir_distance, line_offset);
        task_delay_ms(20);
    }
    /* 到达后退出循环 */
}

/* ====================================================================
 *  Task 2: 巡线控制 (核心控制循环)
 * ==================================================================== */
static void line_follow_task(void)
{
    while (!destination) {
        /* 模拟: 根据线偏移修正方向，小车前进 */
        const char *steer;
        if (line_offset > 3) {
            steer = "<< 向左修正";
            car_speed = (car_speed > 25) ? car_speed - 1 : car_speed;
        } else if (line_offset < -3) {
            steer = ">> 向右修正";
            car_speed = (car_speed > 25) ? car_speed - 1 : car_speed;
        } else {
            steer = "^^ 保持直行";
            car_speed = (car_speed < 30) ? car_speed + 1 : car_speed;
        }

        /* 小车前进一段距离 */
        car_position += car_speed / 10;
        if (car_position > 100) car_position = 100;

        printf("  [巡线控制]  tick=%-3u  位置=%d/100  v=%dcm/s  %s\n",
               tick_count, car_position, car_speed, steer);

        if (car_position >= 100) {
            destination = 1;
            car_speed = 0;
            printf("\n  *** 到达目的地！位置=%d/100  停车！***\n\n", car_position);
        }

        task_delay_ms(40);
    }
}

/* ====================================================================
 *  Task 3: 速度传感器 (定期读取编码器)
 * ==================================================================== */
static void speed_sensor_task(void)
{
    while (!destination) {
        /* 模拟: 速度轻微抖动 */
        int raw_speed = car_speed + ((int)(tick_count % 3) - 1);
        if (raw_speed < 0) raw_speed = 0;

        printf("  [速度传感器] tick=%-3u  编码器读数=%dcm/s  (轮速=%drpm)\n",
               tick_count, raw_speed, raw_speed * 6);
        task_delay_ms(60);
    }
}

/* ====================================================================
 *  Task 4: 加速度计 / IMU (读取三轴加速度)
 * ==================================================================== */
static void imu_task(void)
{
    int count = 0;
    while (!destination) {
        count++;
        /* 模拟: 加速度随运动变化 */
        accel_x = (line_offset > 0 ? 0.12f : -0.08f) + (count % 3) * 0.02f;
        accel_y = ((car_speed - 28) * 0.01f);
        accel_z = 0.98f + (count % 5) * 0.005f;

        printf("  [IMU/加速计] tick=%-3u  ax=%+.2fg  ay=%+.2fg  az=%.2fg\n",
               tick_count, accel_x, accel_y, accel_z);
        task_delay_ms(100);
    }
}

/* ====================================================================
 *  Task 5: 遥测显示 (低优先级，汇总输出)
 * ==================================================================== */
static void telemetry_task(void)
{
    while (!destination) {
        printf("\n  ┌─────────────────────────────────────────┐\n");
        printf("  │      遥  测  数  据  (tick=%-3u)          │\n", tick_count);
        printf("  ├─────────────────────────────────────────┤\n");
        printf("  │  位置:  %3d/100      速度: %3d cm/s       │\n",
               car_position, car_speed);
        printf("  │  红外:  %3d cm       线偏: %+d mm          │\n",
               ir_distance, line_offset);
        printf("  │  加速: X=%+.2fg Y=%+.2fg Z=%.2fg │\n",
               accel_x, accel_y, accel_z);
        printf("  │  状态:  %s                      │\n",
               car_position < 100 ? "巡线行驶中..." : "已到达!");
        printf("  └─────────────────────────────────────────┘\n\n");
        task_delay_ms(200);
    }
}

/* ====================================================================
 *  Task 6: 摄像头 (低优先级 one-shot: 到达后拍照)
 * ==================================================================== */
static void camera_task(void)
{
    while (!destination) {
        task_delay_ms(50);  /* 低频率检查 */
    }

    /* 到达目的地，模拟拍照流程 */
    printf("\n");
    printf("  ╔══════════════════════════════════════╗\n");
    printf("  ║   📷 摄像头触发！                   ║\n");
    printf("  ║                                    ║\n");
    printf("  ║   对焦中...                        ║\n");

    /* 模拟拍照需要时间，分步进行 */
    task_delay_ms(30);
    printf("  ║   曝光中...  (位置=%d, v=0)         ║\n", car_position);
    task_delay_ms(20);
    printf("  ║   图像处理中...                    ║\n");
    task_delay_ms(10);

    photo_serial = 42;
    printf("  ║                                    ║\n");
    printf("  ║   ✅ 拍照完成！                    ║\n");
    printf("  ║   照片编号: IMG_%04d               ║\n", photo_serial);
    printf("  ║   保存路径: /sdcard/IMG_%04d.jpg    ║\n", photo_serial);
    printf("  ╚══════════════════════════════════════╝\n\n");

    printf("=== 小车任务全部完成！=== \n");
    printf("  总行驶距离: %d/100\n", car_position);
    printf("  耗时: %u ticks (约 %.1f 秒)\n",
           tick_count, tick_count * 0.01);
    printf("  传感器采样: IR=%u次  速度=%u次  IMU=%u次\n",
           tick_count / 2, tick_count / 6, tick_count / 10);

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

    printf("\n");
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║     Lite RTOS - 智能小车巡线综合 Demo     ║\n");
    printf("  ╚══════════════════════════════════════════╝\n\n");

    printf("  任务列表:\n");
    printf("  ┌──────────┬──────┬─────────┬──────────────────┐\n");
    printf("  │ 任务     │ 优先级│ 周期    │ 功能             │\n");
    printf("  ├──────────┼──────┼─────────┼──────────────────┤\n");
    printf("  │ IR 传感器│  0   │ 20ms    │ 红外测距 + 线偏移 │\n");
    printf("  │ 巡线控制 │  1   │ 40ms    │ 核心: PID 转向修正│\n");
    printf("  │ 速度传感 │  2   │ 60ms    │ 编码器轮速读取   │\n");
    printf("  │ IMU/加速 │  3   │ 100ms   │ 三轴加速度采集   │\n");
    printf("  │ 遥测显示 │  4   │ 200ms   │ 状态汇总输出     │\n");
    printf("  │ 摄像头   │  5   │ one-shot│ 到达后拍照       │\n");
    printf("  │ Idle     │  7   │  -      │ CPU 空闲         │\n");
    printf("  └──────────┴──────┴─────────┴──────────────────┘\n\n");

    kernel_init();

    /* 设置 tick 间隔: 10ms (直设硬件定时器) */
    kernel_set_tick_interval_ms(10);

    task_create(ir_sensor_task,     "ir",       0, 0);
    task_create(line_follow_task,   "line",     0, 1);
    task_create(speed_sensor_task,  "speed",    0, 2);
    task_create(imu_task,           "imu",      0, 3);
    task_create(telemetry_task,     "telemetry",0, 4);
    task_create(camera_task,        "camera",   0, 5);

    printf("  6 个用户任务已创建，启动调度器...\n\n");
    printf("  ═══════════════════════════════════════════\n\n");

    kernel_start();

    return 0;
}
