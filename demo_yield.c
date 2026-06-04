/**
 * @file    demo_yield.c
 * @brief   Phase 3-4: 合作式调度 + 上下文切换演示
 *
 * 两个同优先级任务，各运行若干次后主动 task_yield() 让出 CPU。
 * 展示上下文切换如何让两个 while(1) 循环在单核上交替执行。
 */

#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
#include "task.h"

static void task_a_func(void)
{
    int count = 0;
    while (1) {
        printf("[Task A] iteration %d\n", count++);
        if (count >= 5) {
            printf("\n=== [Task A] done ===\n");
            exit(0);
        }
        task_yield();  /* 主动让出 CPU */
    }
}

static void task_b_func(void)
{
    int count = 0;
    while (1) {
        printf("[Task B] iteration %d\n", count++);
        if (count >= 5) {
            printf("\n=== [Task B] done ===\n");
            exit(0);
        }
        task_yield();  /* 主动让出 CPU */
    }
}

int main(void)
{
    printf("=== Phase 3-4: 合作式调度 Demo ===\n\n");

    kernel_init();

    task_create(task_a_func, "task_a", 0, 1);
    task_create(task_b_func, "task_b", 0, 1);

    printf("=== 启动调度器 (预期: Task A / Task B 交替打印) ===\n\n");
    kernel_start();

    return 0;
}
