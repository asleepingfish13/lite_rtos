/**
 * @file    port.h
 * @brief   Lite RTOS - 平台移植层头文件
 *
 * Unix (macOS/Linux):  ucontext + setitimer + SIGALRM
 * Windows (MSYS2):     Fibers API + 后台 tick 线程
 */

#ifndef PORT_H
#define PORT_H

#include "kernel.h"

/* ====================================================================
 *  平台相关 API (两种平台同名, 实现不同)
 * ==================================================================== */

/* 上下文切换 */
void port_init_task_context(tcb_t *task, void (*task_func)(void));
void port_start_first_task(tcb_t *task) __attribute__((noreturn));
void port_switch_context(tcb_t *from, tcb_t *to);

/* 系统节拍 */
void port_init_tick_timer(void);
int  port_tick_pending(void);
void port_clear_tick(void);

/* 空闲等待 (模拟 WFI, 避免 100% CPU) */
void port_idle_wait(void);

/* 临界区保护 */
void port_disable_interrupts(void);
void port_enable_interrupts(void);

#endif /* PORT_H */
