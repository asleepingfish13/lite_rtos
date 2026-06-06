/**
 * @file    kernel.h
 * @brief   Lite RTOS - 内核头文件
 *
 * 定义基础类型、任务控制块、任务状态、全局变量声明。
 * 模仿 FreeRTOS 的核心结构。
 *
 * 跨平台:
 *   Unix (macOS/Linux): ucontext.h
 *   Windows (MSYS2):    windows.h (Fibers API)
 */

#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>

/* 平台相关的上下文类型 */
#ifdef _WIN32
    #include <windows.h>
#else
    #include <ucontext.h>
#endif

/* ====================================================================
 *  内核常量
 * ==================================================================== */
#define MAX_PRIORITIES      8       /* 优先级数量: 0(最高) ~ 7(最低/空闲) */
#define IDLE_PRIORITY       (MAX_PRIORITIES - 1)
#define MAX_TASKS           16      /* 默认最大任务数 (可通过 kernel_set_max_tasks() 运行时修改) */
#define DEFAULT_STACK_SIZE       8192    /* 8KB, PC 模拟用较大的栈 */
#define TIME_SLICE_DEFAULT_TICKS 10      /* 默认时间片: 10 ticks (0 = 禁用轮转) */

/* ====================================================================
 *  任务状态
 * ==================================================================== */
#define TASK_READY          0x00
#define TASK_RUNNING        0x01
#define TASK_BLOCKED        0x02    /* 因延时挂起等待 */
#define TASK_SUSPENDED      0x04    /* 主动挂起 */

/* ====================================================================
 *  链表结构 (双向循环链表, FreeRTOS 风格)
 * ==================================================================== */
typedef struct list_node {
    struct list_node *next;
    struct list_node *prev;
} list_node_t;

typedef struct list {
    list_node_t sentinel;           /* 哨兵节点, 标记链表头尾 */
} list_t;

/* ====================================================================
 *  任务控制块 (TCB)
 * ==================================================================== */
typedef struct tcb {
    /* 上下文信息 */
#ifdef _WIN32
    void               *fiber;         /* Windows: Fiber 句柄 */
#else
    ucontext_t          context;        /* Unix: ucontext 保存的 CPU 上下文 */
#endif
    uint8_t            *stack;          /* 任务栈起始地址 (用于 free) */
    uint32_t            stack_size;     /* 栈大小 */

    /* 任务属性 */
    uint8_t             priority;       /* 当前优先级 */
    uint8_t             base_priority;  /* 基础优先级 (预留给互斥量) */
    uint8_t             state;          /* 任务状态 */
    char                name[16];       /* 任务名称 (调试用) */

    /* 延时信息 */
    uint32_t            delay_ticks;    /* 延时还需等待的 tick 数 */

    /* 时间片信息 */
    uint32_t            time_slice_ticks; /* 剩余时间片 tick 数 (0 = 不参与轮转) */

    /* 链表节点 (嵌入结构体, 挂载到就绪列表或延时列表) */
    list_node_t         node;
} tcb_t;

/* ====================================================================
 *  全局变量声明 (定义在 kernel.c)
 * ==================================================================== */
extern tcb_t   *current_task;                   /* 当前正在运行的任务 */
extern list_t   ready_lists[MAX_PRIORITIES];    /* 就绪链表组 */
extern list_t   delayed_list;                   /* 延时任务链表 */
extern tcb_t    idle_task_tcb;                  /* 空闲任务 TCB */
extern uint32_t tick_count;                     /* 系统节拍计数 */
extern uint8_t  scheduler_running;              /* 调度器是否已启动 */
extern uint32_t  g_max_tasks;                   /* 运行时最大任务数 (可通过 API 修改) */
extern uint32_t  g_task_count;                  /* 当前已创建的任务数 */
extern uint32_t  g_time_slice_ticks;           /* 运行时时间片配置 (可通过 API 修改) */

/* ====================================================================
 *  宏: 从链表节点反查结构体指针 (FreeRTOS 风格)
 * ==================================================================== */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

/* 遍历链表 */
#define list_for_each(pos, list) \
    for (pos = (list)->sentinel.next; pos != &(list)->sentinel; pos = pos->next)

#define list_for_each_safe(pos, tmp, list) \
    for (pos = (list)->sentinel.next, tmp = pos->next; \
         pos != &(list)->sentinel; \
         pos = tmp, tmp = pos->next)

/* ====================================================================
 *  内核 API
 * ==================================================================== */
void kernel_init(void);
void kernel_start(void);
void schedule(void);
void schedule_from_isr(void);   /* ISR/信号处理函数入口 */
void task_yield(void);

/* 任务数量上限 (运行时可变) */
void     kernel_set_max_tasks(uint32_t max);    /* 设置最大任务数 (>=1, 已创建任务数不受影响) */
uint32_t kernel_get_max_tasks(void);            /* 获取当前最大任务数 */
uint32_t task_get_count(void);                  /* 获取当前已创建的任务数 */

/* 系统节拍配置 */
void     kernel_set_tick_interval_ms(uint32_t ms);  /* 设置 tick 间隔: 1~1000ms (直设硬件定时器) */
uint32_t kernel_get_tick_interval_ms(void);         /* 获取 tick 间隔 (ms) */
uint32_t kernel_get_tick_rate(void);                /* 获取当前 tick 频率 (Hz), 内部使用 */
void     kernel_set_tick_rate(uint32_t hz);         /* 设置 tick 频率 (Hz), 内部使用 */

/* 时间片配置 (仅对同优先级任务有效, 0 = 禁用轮转) */
void     kernel_set_time_slice(uint32_t ticks);
uint32_t kernel_get_time_slice(void);

#include "sem.h"
#include "perf.h"

#endif /* KERNEL_H */
