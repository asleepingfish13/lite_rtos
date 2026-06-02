/**
 * @file    list.h
 * @brief   Lite RTOS - 双向循环链表 (FreeRTOS 风格)
 *
 * 侵入式链表: 链表节点嵌入在数据结构内部，
 * 不需要额外的内存分配，也避免了缓存不友好。
 *
 * 设计要点:
 *   - 哨兵节点 (sentinel) 标记头尾，简化边界判断
 *   - 所有操作 O(1)
 *   - 节点不持有数据指针，使用时通过 list_entry 反查容器
 */

#ifndef LIST_H
#define LIST_H

#include "kernel.h"

/* ====================================================================
 *  链表 API
 * ==================================================================== */

/**
 * 初始化链表
 * 空链表: sentinel.next = sentinel.prev = &sentinel
 */
void list_init(list_t *list);

/**
 * 在链表尾部插入节点 (sentinel 之前)
 */
void list_insert_tail(list_t *list, list_node_t *node);

/**
 * 在链表头部插入节点 (sentinel 之后)
 */
void list_insert_head(list_t *list, list_node_t *node);

/**
 * 从链表中移除节点 (节点自身不需要知道属于哪个链表)
 */
void list_remove(list_node_t *node);

/**
 * 判断链表是否为空
 * @return 1=空, 0=非空
 */
int list_is_empty(const list_t *list);

/**
 * 获取链表第一个节点 (不删除)
 * @return 第一个节点指针, 链表空则返回 NULL
 */
list_node_t *list_first(const list_t *list);

/**
 * 获取链表最后一个节点 (不删除)
 * @return 最后一个节点指针, 链表空则返回 NULL
 */
list_node_t *list_last(const list_t *list);

/**
 * 弹出链表头部节点 (移除并返回)
 * @return 弹出的节点指针, 链表空则返回 NULL
 */
list_node_t *list_pop_head(list_t *list);

#endif /* LIST_H */
