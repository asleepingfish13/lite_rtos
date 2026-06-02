/**
 * @file    list.c
 * @brief   Lite RTOS - 双向循环链表实现
 *
 * 所有操作 O(1), 线程不安全 (在内核内部使用, 由调度器保证互斥)。
 */

#include "list.h"

/* -------------------------------------------------------------------
 *  list_init - 初始化链表
 *
 *  初始状态:
 *    [sentinel] ←→ [sentinel]   (自己指回自己)
 * ------------------------------------------------------------------- */
void list_init(list_t *list)
{
    list->sentinel.next = &list->sentinel;
    list->sentinel.prev = &list->sentinel;
}

/* -------------------------------------------------------------------
 *  list_insert_tail - 在链表尾部插入节点
 *
 *  插入前:  ... ←→ [prev] ←→ [sentinel]
 *  插入后:  ... ←→ [prev] ←→ [node] ←→ [sentinel]
 * ------------------------------------------------------------------- */
void list_insert_tail(list_t *list, list_node_t *node)
{
    /* 先连接 node 的左右 */
    node->next = &list->sentinel;
    node->prev = list->sentinel.prev;

    /* 再让邻居指向 node (顺序不能错!) */
    list->sentinel.prev->next = node;
    list->sentinel.prev = node;
}

/* -------------------------------------------------------------------
 *  list_insert_head - 在链表头部插入节点
 *
 *  插入前:  [sentinel] ←→ [next] ←→ ...
 *  插入后:  [sentinel] ←→ [node] ←→ [next] ←→ ...
 * ------------------------------------------------------------------- */
void list_insert_head(list_t *list, list_node_t *node)
{
    node->prev = &list->sentinel;
    node->next = list->sentinel.next;

    list->sentinel.next->prev = node;
    list->sentinel.next = node;
}

/* -------------------------------------------------------------------
 *  list_remove - 从链表中移除节点
 *
 *  只需要节点的 prev 和 next，不需要知道属于哪个链表。
 *  设计精妙之处: O(1) 删除，不需要遍历!
 * ------------------------------------------------------------------- */
void list_remove(list_node_t *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;

    /* 清理悬空指针 (可选, 帮助调试) */
    node->next = NULL;
    node->prev = NULL;
}

/* -------------------------------------------------------------------
 *  list_is_empty - 判断链表是否为空
 *
 *  空链表: sentinel 的 next 指向自己
 * ------------------------------------------------------------------- */
int list_is_empty(const list_t *list)
{
    return (list->sentinel.next == &list->sentinel);
}

/* -------------------------------------------------------------------
 *  list_first - 获取链表第一个节点
 * ------------------------------------------------------------------- */
list_node_t *list_first(const list_t *list)
{
    if (list_is_empty(list)) {
        return NULL;
    }
    return list->sentinel.next;
}

/* -------------------------------------------------------------------
 *  list_last - 获取链表最后一个节点
 * ------------------------------------------------------------------- */
list_node_t *list_last(const list_t *list)
{
    if (list_is_empty(list)) {
        return NULL;
    }
    return list->sentinel.prev;
}

/* -------------------------------------------------------------------
 *  list_pop_head - 弹出链表头部节点 (移除并返回)
 * ------------------------------------------------------------------- */
list_node_t *list_pop_head(list_t *list)
{
    list_node_t *node = list_first(list);
    if (node != NULL) {
        list_remove(node);
    }
    return node;
}
