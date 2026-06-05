/**
 * @file    sem.h
 * @brief   Lite RTOS - 计数信号量 (Counting Semaphore)
 *
 * 在现有任务阻塞机制 (task_delay) 之上构建的同步原语。
 * 复用 TCB 的 node 成员将任务挂载到信号量的 wait_list，
 * 被阻塞的任务从 ready_lists[] 移除，被 sem_give 唤醒后放回。
 *
 * 设计要点:
 *   - FIFO 唤醒顺序 (list_pop_head)
 *   - sem_give 仅在唤醒任务优先级高于当前任务时才调用 schedule()
 *   - 支持 ISR 上下文 (sem_give_from_isr, 不调用 schedule)
 *   - 临界区保护 (port_disable/port_enable, 嵌套安全)
 *   - TCB 单节点约束: 不支持带超时的等待 (需要第二个 list_node_t)
 */

#ifndef SEM_H
#define SEM_H

#include "kernel.h"

/* ====================================================================
 *  信号量结构
 * ==================================================================== */
typedef struct {
    int     count;       /* 可用资源计数 (>= 0) */
    list_t  wait_list;   /* 阻塞在此信号量上的任务队列 (FIFO) */
} sem_t;

/* ====================================================================
 *  API
 * ==================================================================== */

/**
 * 初始化信号量
 * @param sem           信号量指针
 * @param initial_count 初始计数 (>= 0)
 */
void sem_init(sem_t *sem, int initial_count);

/**
 * 阻塞获取信号量
 * 若 count > 0 则立即返回 (递减 count)，
 * 否则阻塞当前任务直到 sem_give 唤醒。
 * 被唤醒后直接返回 (令牌已由 give 直接转移, 不重复递减 count)。
 */
void sem_take(sem_t *sem);

/**
 * 非阻塞尝试获取信号量
 * @return 1=成功获取 (count 已递减), 0=资源不可用
 */
int  sem_try_take(sem_t *sem);

/**
 * 释放信号量 (任务上下文)
 * 若有等待者则唤醒队首任务 (FIFO)，
 * 否则递增 count。
 * 唤醒任务优先级高于当前任务时调用 schedule()。
 */
void sem_give(sem_t *sem);

/**
 * 释放信号量 (ISR 上下文)
 * 与 sem_give 相同但不调用 schedule()，
 * 由 ISR 框架 (schedule_from_isr) 统一处理调度。
 */
void sem_give_from_isr(sem_t *sem);

#endif /* SEM_H */
