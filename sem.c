/**
 * @file    sem.c
 * @brief   Lite RTOS - 计数信号量实现
 *
 * 核心逻辑复用 task_delay() 的阻塞模式:
 *   task_remove_from_ready → TASK_BLOCKED → 插入等待队列 → schedule()
 *
 * 唤醒复用 process_tick() 的模式:
 *   list_remove (从等待队列) → task_add_to_ready
 */

#include "sem.h"
#include "list.h"
#include "task.h"
#include "port.h"

/* 外部声明 (kernel.c) */
extern tcb_t *current_task;
extern void   schedule(void);

/* ====================================================================
 *  sem_init
 * ==================================================================== */
void sem_init(sem_t *sem, int initial_count)
{
    sem->count = initial_count;
    list_init(&sem->wait_list);
}

/* ====================================================================
 *  sem_take — 阻塞获取信号量
 *
 *  两条路径:
 *    快路径: count > 0 → 递减 count, 立即返回
 *    慢路径: count == 0 → 阻塞当前任务, 切换走
 *
 *  慢路径与 task_delay() 的 5 步模式完全相同:
 *    ① 离开就绪链表
 *    ② 标记 TASK_BLOCKED
 *    ③ 插入信号量等待队列 (而非 delayed_list)
 *    ④ 开中断
 *    ⑤ schedule() 切走
 *
 *  被 sem_give 唤醒后从此处继续执行。
 *  令牌已由 sem_give 直接转移给本任务 (唤醒而非 count++),
 *  故无需递减 count — 直接 return。
 * ==================================================================== */
void sem_take(sem_t *sem)
{
    port_disable_interrupts();

    /* 快路径: 资源可用 */
    if (sem->count > 0) {
        sem->count--;
        port_enable_interrupts();
        return;
    }

    /* 慢路径: 资源不可用, 阻塞 */
    task_remove_from_ready(current_task);
    current_task->state = TASK_BLOCKED;
    list_insert_tail(&sem->wait_list, &current_task->node);

    port_enable_interrupts();
    schedule();

    /* 被 sem_give 唤醒后从此处继续。
     * sem_give 发现 wait_list 非空时直接唤醒了队首任务,
     * 而非递增 count。令牌已"转交"给本任务, 无需再递减 count。 */
}

/* ====================================================================
 *  sem_try_take — 非阻塞尝试获取
 *
 *  资源可用: 递减 count, 返回 1
 *  资源不可用: 返回 0, 不阻塞
 * ==================================================================== */
int sem_try_take(sem_t *sem)
{
    port_disable_interrupts();

    if (sem->count > 0) {
        sem->count--;
        port_enable_interrupts();
        return 1;
    }

    port_enable_interrupts();
    return 0;
}

/* ====================================================================
 *  sem_give — 释放信号量 (任务上下文)
 *
 *  两条路径:
 *    有等待者: 唤醒队首任务 (FIFO), 令牌直接转移
 *    无等待者: 递增 count, 令牌暂存
 *
 *  优先级检查:
 *    仅在唤醒任务优先级高于当前任务时才调用 schedule(),
 *    避免高优先级任务刚 give 就被低优先级任务抢占。
 * ==================================================================== */
void sem_give(sem_t *sem)
{
    tcb_t *woken = NULL;

    port_disable_interrupts();

    if (!list_is_empty(&sem->wait_list)) {
        /* 有任务在等待: 唤醒队首 (FIFO) */
        list_node_t *node = list_pop_head(&sem->wait_list);
        woken = list_entry(node, tcb_t, node);
        task_add_to_ready(woken);
    } else {
        /* 无等待者: count 递增 */
        sem->count++;
    }

    port_enable_interrupts();

    /* 仅当唤醒任务的优先级高于当前任务时才触发调度 */
    if (woken != NULL && woken->priority < current_task->priority) {
        schedule();
    }
}

/* ====================================================================
 *  sem_give_from_isr — 释放信号量 (ISR 上下文)
 *
 *  与 sem_give 相同, 但不调用 schedule()。
 *  ISR 框架 (schedule_from_isr → schedule_select_and_switch)
 *  会在 ISR 退出时统一处理调度, 无需在此重复。
 * ==================================================================== */
void sem_give_from_isr(sem_t *sem)
{
    port_disable_interrupts();

    if (!list_is_empty(&sem->wait_list)) {
        list_node_t *node = list_pop_head(&sem->wait_list);
        tcb_t *woken = list_entry(node, tcb_t, node);
        task_add_to_ready(woken);
    } else {
        sem->count++;
    }

    port_enable_interrupts();
    /* 不调用 schedule() — 由 ISR 框架统一调度 */
}
