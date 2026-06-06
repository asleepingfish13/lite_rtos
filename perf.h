/**
 * @file    perf.h
 * @brief   Lite RTOS - 性能检测与实时性验证模块
 *
 * 通过在内核关键路径插入轻量级钩子，
 * 量化测量 RTOS 的实时性指标：
 *   1. 任务响应时间 (wake-up → 实际运行)
 *   2. 上下文切换时间
 *   3. 抢占延迟与次数
 *   4. 任务执行时间
 *   5. 周期抖动
 *   6. CPU 利用率
 *   7. 截止期错失检测
 *
 * 所有钩子由 g_perf.initialized 守卫，
 * 未初始化时几乎零开销 (单次 if 判断)。
 */

#ifndef PERF_H
#define PERF_H

#include <stdint.h>

/* ====================================================================
 *  常量
 * ==================================================================== */
#define PERF_MAX_TASKS          20      /* 最多追踪 20 个任务 */
#define PERF_NAME_LEN           16      /* 任务名最大长度 */
#define PERF_MAX_PRIORITIES     8       /* 优先级数量 */

/* ====================================================================
 *  每任务性能统计
 * ==================================================================== */
typedef struct {
    uint8_t  active;                    /* 此槽位是否被占用 */

    /* 基本计数 */
    uint32_t run_count;                 /* 任务被调度运行的次数 */
    uint32_t preempt_count;             /* 被抢占次数 */
    uint32_t preempted_by[PERF_MAX_PRIORITIES]; /* 被各优先级抢占的次数分布 */

    /* 响应时间 (wake → run)，单位 ns
     *   从任务延时到期 (被唤醒) 到实际开始运行的时间差。
     *   这是实时性最核心的指标。 */
    uint64_t response_min_ns;
    uint64_t response_max_ns;
    uint64_t response_total_ns;
    uint32_t response_count;

    /* 执行时间 (run → switch_out)，单位 ns
     *   任务一次连续占用的 CPU 时间。 */
    uint64_t exec_min_ns;
    uint64_t exec_max_ns;
    uint64_t exec_total_ns;

    /* 周期统计 (连续两次 run_start 的时间差)，单位 ns
     *   对于周期性任务，测量实际周期与目标周期的偏差 (抖动)。 */
    uint64_t period_target_ns;          /* 目标周期 (0 = 未设置) */
    uint64_t period_min_ns;
    uint64_t period_max_ns;
    uint64_t period_total_ns;
    uint32_t period_count;

    /* 截止期 */
    uint32_t deadline_misses;           /* 执行时间超过目标周期的次数 */

    /* ---- 内部追踪用 (不对外) ---- */
    uint64_t last_wake_ns;              /* 最近一次被唤醒的时间戳 */
    uint64_t last_start_ns;             /* 最近一次开始运行的时间戳 */
    uint64_t last_period_start_ns;      /* 上一个周期起始时间戳 */
    char     name[PERF_NAME_LEN];       /* 任务名 (复制自 TCB) */
    uint8_t  priority;                  /* 任务优先级 */
} perf_task_t;

/* ====================================================================
 *  全局性能统计
 * ==================================================================== */
typedef struct {
    uint8_t  initialized;               /* 是否已初始化 (钩子守卫) */
    uint64_t init_time_ns;              /* perf_init() 调用时刻 */

    /* 上下文切换统计 */
    uint32_t cs_count;                  /* 上下文切换总次数 */
    uint64_t cs_total_ns;               /* 上下文切换总耗时 (ns) */
    uint64_t cs_min_ns;                 /* 最快速的一次上下文切换 */
    uint64_t cs_max_ns;                 /* 最慢的一次上下文切换 */
    uint64_t cs_pending_start_ns;       /* CS 计时起始 (内部) */
    void    *cs_pending_from;           /* 当前 CS 的 from 任务 (内部) */
    void    *cs_pending_to;             /* 当前 CS 的 to 任务 (内部) */

    /* 抢占统计 */
    uint32_t total_preemptions;         /* 系统总抢占次数 */

    /* 每任务统计 */
    uint32_t    task_count;             /* 已注册的任务数 */
    perf_task_t tasks[PERF_MAX_TASKS];  /* 任务统计数组 */
} perf_global_t;

/* 全局实例 (定义在 perf.c) */
extern perf_global_t g_perf;

/* ====================================================================
 *  API 函数
 * ==================================================================== */

/* 初始化 / 重置 */
void perf_init(void);
void perf_reset(void);

/* ---- 内核钩子 (由 kernel.c / task.c 调用) ---- */

/**
 * 任务创建时调用，在全局表中注册该任务
 */
void perf_hook_task_create(void *task);

/**
 * 任务延时到期、被唤醒时调用，记录 wake 时间戳
 */
void perf_hook_task_wake(void *task);

/**
 * port_switch_context() 之前调用，记录 CS 开始时间 + from/to
 */
void perf_hook_switch_start(void *from, void *to);

/**
 * port_switch_context() 之后调用（在新任务上下文中执行）
 * 计算 CS 耗时、执行时间、响应时间、周期统计。
 * 无参数 — 通过内部全局变量获取当前切换的 from/to。
 * （不能传局部变量参数，因为 swapcontext 后局部变量属于被恢复的任务）
 */
void perf_hook_switch_end(void);

/**
 * 发生抢占时调用
 */
void perf_hook_preempt(void *preempted, void *by);

/* ---- 任务端 API ---- */

/**
 * 设置任务的目标周期 (用于抖动和截止期检测)
 * @param name       任务名
 * @param period_ns  目标周期 (纳秒)
 */
void perf_task_set_period(const char *name, uint64_t period_ns);

/* ---- 报告 ---- */

/**
 * 打印完整性能报告 (含实时性验证结论)
 */
void perf_print_report(void);

/**
 * 打印简要摘要 (单行)
 */
void perf_print_summary(void);

#endif /* PERF_H */
