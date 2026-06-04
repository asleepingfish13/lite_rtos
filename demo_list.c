/**
 * @file    demo_list.c
 * @brief   Phase 1: 侵入式双向循环链表 (Intrusive Linked List) 演示
 *
 * 单独测试链表操作，不涉及 RTOS 内核。
 * 理解 container_of、哨兵节点、O(1) 删除等核心概念。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"

/* 定义一个简单的测试结构体，内含 list_node_t */
typedef struct {
    int           id;
    char          name[32];
    list_node_t   node;  /* 侵入式链表节点 */
} test_item_t;

int main(void)
{
    printf("=== Phase 1: 侵入式双向循环链表 Demo ===\n\n");

    /* ---- 1. 初始化链表 ---- */
    list_t my_list;
    list_init(&my_list);
    printf("[1] 空链表初始化完成 (sentinel 的 next/prev 都指向自己)\n");
    printf("    list_is_empty: %s\n\n", list_is_empty(&my_list) ? "YES" : "NO");

    /* ---- 2. 创建几个测试项并插入链表 ---- */
    test_item_t items[3];
    for (int i = 0; i < 3; i++) {
        items[i].id = i + 1;
        snprintf(items[i].name, sizeof(items[i].name), "Item-%d", i + 1);
    }

    /* 头部插入 Item-2, 尾部插入 Item-1, 尾部插入 Item-3 */
    list_insert_head(&my_list, &items[1].node); /* Item-2 */
    list_insert_tail(&my_list, &items[0].node); /* Item-1 */
    list_insert_tail(&my_list, &items[2].node); /* Item-3 */

    printf("[2] 插入后链表内容 (sentinel -> Item-2 -> Item-1 -> Item-3 -> sentinel):\n");
    printf("    ");
    list_node_t *n;
    list_for_each(n, &my_list) {
        test_item_t *item = list_entry(n, test_item_t, node);
        printf("[id=%d, name=%s] -> ", item->id, item->name);
    }
    printf("sentinel\n\n");

    /* ---- 3. 演示 container_of ---- */
    printf("[3] container_of 原理演示:\n");
    printf("    offsetof(test_item_t, node) = %u 字节\n",
           (unsigned)offsetof(test_item_t, node));
    printf("    items[0] 起始地址: %p\n", (void *)&items[0]);
    printf("    items[0].node 地址: %p\n", (void *)&items[0].node);
    printf("    container_of(&items[0].node) == &items[0] ? %s\n\n",
           (void *)list_entry(&items[0].node, test_item_t, node) == (void *)&items[0]
               ? "YES (正确!)" : "NO");

    /* ---- 4. 演示 O(1) 删除 (不需要遍历，不需要知道属于哪个链表) ---- */
    printf("[4] O(1) 删除 Item-1 (不需要遍历链表):\n");
    list_remove(&items[0].node); /* 直接从链表中摘掉 Item-1 */
    printf("    删除后链表: ");
    list_for_each(n, &my_list) {
        test_item_t *item = list_entry(n, test_item_t, node);
        printf("[%s] -> ", item->name);
    }
    printf("sentinel\n\n");

    /* ---- 5. 演示 list_pop_head ---- */
    printf("[5] 弹出链表头 (Item-2):\n");
    list_node_t *popped = list_pop_head(&my_list);
    test_item_t *pitem = list_entry(popped, test_item_t, node);
    printf("    弹出: id=%d, name=%s\n", pitem->id, pitem->name);
    printf("    剩余: ");
    list_for_each(n, &my_list) {
        test_item_t *item = list_entry(n, test_item_t, node);
        printf("[%s] -> ", item->name);
    }
    printf("sentinel\n\n");

    printf("=== Phase 1 Demo 结束 ===\n");
    printf("核心要点:\n");
    printf("  1. 侵入式链表: 节点嵌入数据结构，链表不拥有数据\n");
    printf("  2. container_of: 从 node 地址反推结构体地址\n");
    printf("  3. 哨兵节点: 消除空链表边界情况，首尾相连\n");
    printf("  4. O(1) 删除: list_remove() 不需要遍历，不需要知道属于哪个链表\n");
    return 0;
}
