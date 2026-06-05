/**
 * @file    task.h
 * @brief   Lite RTOS - 任务管理 API
 */

#ifndef TASK_H
#define TASK_H

#include "kernel.h"

/* ====================================================================
 *  API 函数
 * ==================================================================== */

tcb_t *task_create(void (*task_func)(void),
                   const char *name,
                   uint32_t stack_size,
                   uint8_t priority);

void task_add_to_ready(tcb_t *task);
void task_remove_from_ready(tcb_t *task);

/**
 * 任务延时 (阻塞当前任务指定 tick 数)
 *
 * 调用后当前任务进入 BLOCKED 状态，
 * 从就绪列表移除，加入延时列表。
 * 经过 ticks 个系统节拍后自动唤醒。
 *
 * @param ticks  延时 tick 数 (依赖于系统节拍频率)
 */
void task_delay(uint32_t ticks);

/**
 * 任务延时 — 毫秒版本 (推荐)
 *
 * 等同于 task_delay(), 但参数是真实毫秒数,
 * 与当前 tick 间隔无关。内部自动换算为 tick 数。
 *
 * 例如: tick 间隔=10ms 时, task_delay_ms(50) = task_delay(5)
 *       tick 间隔=1ms 时, task_delay_ms(50) = task_delay(50)
 *
 * @param ms  延时毫秒数
 */
void task_delay_ms(uint32_t ms);

#endif /* TASK_H */
