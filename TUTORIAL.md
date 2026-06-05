# Lite RTOS 从零学习教程

> 一个模仿 FreeRTOS 实现的微型实时操作系统内核，总计约 930 行 C 代码。
> 本文档带你从零开始理解每一行代码的设计意图。

---

## 目录

1. [概述：实时操作系统解决什么问题？](#1-概述实时操作系统解决什么问题)
2. [整体架构一览](#2-整体架构一览)
3. [Phase 1：基础数据结构和侵入式链表](#3-phase-1基础数据结构和侵入式链表)
4. [Phase 2：任务控制块与任务创建](#4-phase-2任务控制块与任务创建)
5. [Phase 3：上下文切换的原理与实现](#5-phase-3上下文切换的原理与实现)
6. [Phase 4：优先级调度器](#6-phase-4优先级调度器)
7. [Phase 5：系统节拍与任务延时](#7-phase-5系统节拍与任务延时)
8. [Phase 6：抢占式调度（延迟抢占）](#8-phase-6抢占式调度延迟抢占)
9. [Phase 7：真抢占式调度（ISR 驱动）](#9-phase-7真抢占式调度isr-驱动)
10. [完整执行流程追踪](#10-完整执行流程追踪)
11. [与 FreeRTOS 的对比](#11-与-freertos-的对比)
12. [扩展方向](#12-扩展方向)

---

## 1. 概述：实时操作系统解决什么问题？

### 裸机编程的困境

在没有操作系统的单片机程序（裸机）中，你通常只有一个 `while(1)` 超级循环：

```c
void main(void) {
    while (1) {
        read_sensor();      // 读传感器 (耗时 5ms)
        update_display();   // 刷新屏幕 (耗时 50ms)
        check_button();     // 检测按键 (耗时 1ms)
        send_heartbeat();   // 发送心跳包 (每 100ms 需要一次)
    }
}
```

**问题**：
- 如果 `update_display()` 耗时太久，`check_button()` 响应迟钝
- `send_heartbeat()` 无法保证精确的 100ms 间隔
- 所有任务按固定顺序执行，无法按紧急程度排列

### RTOS 的解决方案

RTOS 让**每个任务看起来独占 CPU**，由内核决定"谁该运行"：

```c
void task_sensor(void) {    // 高优先级
    while (1) { read_sensor(); task_delay(5); }
}
void task_display(void) {   // 低优先级
    while (1) { update_display(); task_delay(50); }
}
void task_heartbeat(void) { // 最高优先级
    while (1) { send_heartbeat(); task_delay(100); }
}
```

内核自动确保高优先级任务能**抢占**低优先级任务——这就是"实时"的含义。

---

## 2. 整体架构一览

```
┌─────────────────────────────────────────────────────────┐
│                     Application                         │
│            (task_a, task_b, idle task...)                │
├─────────────────────────────────────────────────────────┤
│                   Lite RTOS Kernel                       │
│                                                         │
│   ┌─────────────┐  ┌──────────────┐  ┌──────────────┐  │
│   │  task.c      │  │  kernel.c    │  │  port.c      │  │
│   │  ─────────  │  │  ──────────  │  │  ────────    │  │
│   │  任务创建    │  │  调度器       │  │  上下文切换   │  │
│   │  延时管理    │  │  Tick 处理   │  │  中断控制     │  │
│   │  就绪列表    │  │  内核初始化   │  │  Tick 定时器  │  │
│   └──────┬──────┘  └──────┬───────┘  └──────┬───────┘  │
│          │                │                  │          │
│   ┌──────┴────────────────┴──────────────────┴───────┐  │
│   │              list.c — 侵入式双向循环链表           │  │
│   │         (所有内核数据结构的基础设施)               │  │
│   └──────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────┤
│               PC 模拟层 (macOS / Linux)                  │
│     ucontext.h (上下文)  +  setitimer (节拍)            │
│                  +  SIGALRM (中断)                       │
└─────────────────────────────────────────────────────────┘
```

### 模块职责

| 文件 | 职责 | 类比 FreeRTOS |
|------|------|--------------|
| `kernel.h` | 类型定义、TCB 结构、全局变量声明、API 原型 | `FreeRTOS.h`, `task.h` |
| `kernel.c` | 内核初始化、`schedule()`、tick 处理、`task_yield()` | `tasks.c` |
| `list.h/c` | 双向循环链表（侵入式） | `list.h/c` |
| `task.h/c` | `task_create()`、`task_delay()`、就绪列表操作 | `tasks.c` |
| `port.h/c` | ucontext 上下文切换、SIGALRM 系统节拍、临界区 | `port.c` (移植层) |
| `demo.c` | 示例应用 | 你的 `main.c` |

---

## 跨平台支持：一个 Makefile，两个平台

Lite RTOS 用一个 Makefile + 一个 `port.c` 同时支持两种平台。

### 平台对比

| | Unix (macOS / Linux) | Windows (MSYS2) |
|---|---|---|
| **上下文切换** | `ucontext` (`swapcontext`) | Fibers API (`SwitchToFiber`) |
| **系统节拍** | `setitimer` + `SIGALRM` 信号 | 后台线程 + `Sleep(10ms)` |
| **临界区** | 阻塞 `SIGALRM` 信号 | 无需 (tick 线程只设标志) |
| **编译标志** | `-D_XOPEN_SOURCE=600` | 无额外标志 |
| **可执行扩展名** | 无 | `.exe` |
| **Make 可用?** | ✅ 系统自带 | ✅ MSYS2 提供 |

### Makefile 自动检测逻辑

```makefile
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)          # macOS
    CFLAGS += -D_XOPEN_SOURCE=600 -Wno-deprecated-declarations
else ifneq ($(findstring MINGW,$(UNAME_S)),)  # Windows MSYS2
    EXE_EXT := .exe
    # Windows 不需要特殊 CFLAGS
endif
```

### port.c 中的平台切换

```c
#ifdef _WIN32
    // Windows: Fibers API
    void port_start_first_task(tcb_t *task) {
        ConvertThreadToFiber(NULL);    // 当前线程 → Fiber
        SwitchToFiber(task->fiber);    // 跳转到第一个任务
    }
    void port_switch_context(tcb_t *from, tcb_t *to) {
        SwitchToFiber(to->fiber);      // 切换到目标 Fiber
    }
#else
    // Unix: ucontext
    void port_start_first_task(tcb_t *task) {
        setcontext(&task->context);    // 跳转到第一个任务
    }
    void port_switch_context(tcb_t *from, tcb_t *to) {
        swapcontext(&from->context, &to->context);  // 保存 + 切换
    }
#endif
```

### Windows 上编译

```bash
# 在 MSYS2 终端中:
cd /c/Users/xxx/lite_rtos
make clean && make && make run
```

其余 99% 的代码（`kernel.c`, `task.c`, `list.c`, `demo.c`）完全不用改——这就是移植层的价值。

---

## 3. Phase 1：基础数据结构和侵入式链表

### 推荐先读这些文件

> `kernel.h` → `list.h` → `list.c`

### 3.1 类型定义 ([kernel.h](kernel.h))

```c
#define MAX_PRIORITIES      8       // 支持 0~7 共 8 个优先级，0 最高
#define IDLE_PRIORITY       (MAX_PRIORITIES - 1)  // 空闲任务优先级 = 7 (最低)
#define MAX_TASKS           16
#define DEFAULT_STACK_SIZE  8192    // 每个任务 8KB 栈 (PC 上给大一些)
```

任务状态：
```
TASK_READY    (0x00) ──→ 在就绪链表中，等待 CPU
TASK_RUNNING  (0x01) ──→ 正在使用 CPU
TASK_BLOCKED  (0x02) ──→ 在延时链表中，等待时间到达
TASK_SUSPENDED(0x04) ──→ 主动挂起 (暂未使用，预留给后续扩展)
```

状态转换：
```
                task_create()
  [不存在] ───────────────────→ [READY]
                                    │
                            schedule() 选择
                                    │
                                    ▼
                                [RUNNING] ──→ task_yield() ──→ [READY]
                                    │
                              task_delay()
                                    │
                                    ▼
                                [BLOCKED] ──→ tick 到期 ──→ [READY]
```

### 3.2 侵入式链表 ([list.h](list.h), [list.c](list.c))

这是整个内核最重要的数据结构。理解它，就理解了 FreeRTOS 的一半。

**普通链表 vs 侵入式链表**：

```c
// 普通链表：数据包含节点指针
struct normal_list {
    int    data;
    struct normal_list *next;  // next 指向整个节点
};

// 侵入式链表：节点嵌入数据结构
struct tcb {
    ...
    list_node_t node;   // 链表节点是结构体的一个成员
    ...
};
```

**为什么 FreeRTOS 用侵入式链表？**

```c
/* 普通链表删除：需要知道属于哪个链表，O(n) 遍历 */
void list_delete(list_t *list, node_t *target) {
    node_t *prev = NULL;
    node_t *cur = list->head;
    while (cur != target) { prev = cur; cur = cur->next; } // O(n)
    prev->next = cur->next;
    free(cur);
}

/* 侵入式链表删除：只需要节点自身，O(1)！ */
void list_remove(list_node_t *node) {
    node->prev->next = node->next;   // O(1)
    node->next->prev = node->prev;   // O(1)
    // 不需要 free，节点是嵌入的
}
```

侵入式链表的精髓：**链表不拥有数据，数据拥有链表节点**。删除时不需要遍历，也不需要知道属于哪个链表——只需要节点的 `prev` 和 `next` 指针。

**深入理解：两种链表的内存布局**

```
普通链表 — 数据"拥有"节点指针：
  malloc 出来的整块数据               malloc 出来的整块数据
  ┌─────────────────────┐             ┌─────────────────────┐
  │ data=42             │   next      │ data=99             │
  │ next ───────────────┼────────────→│ next = NULL         │
  └─────────────────────┘             └─────────────────────┘
  next 指向的是"下一个数据块的起始地址"。数据块和链表指针是同一次 malloc 的。

侵入式链表 — 节点"嵌入"数据结构：
  每个 TCB 在内存中是静态的（创立后就放在那不动），链表只串起每个 TCB 里的 node 成员：

  TCB_A 起始地址                           TCB_B 起始地址
  ┌──────────────────────┐                ┌──────────────────────┐
  │ name | priority | ... │                │ name | priority | ... │
  │ node.next ───────────┼──→ ... ──→     │ node.next ────────────┼──→ sentinel
  │ node.prev ←──────────┼── ... ←──     │ node.prev ←───────────┼── ...
  └──────────────────────┘                └──────────────────────┘

  链表串起来的不是整个 TCB，而是每个 TCB 里那个小小的 node 成员。
```

**调度器在做什么？只是在改指针：**

所有 TCB 在内存里创建后就放在那不动。调度器的全部工作就是改 `node.prev` 和 `node.next` 指针，重新排列串联顺序：

```
就绪链表:   sentinel → [TCB_A.node] → [TCB_C.node] → sentinel
延时链表:   sentinel → [TCB_B.node] → sentinel

当 Task B 的 tick 到期：
  1. list_remove(&TCB_B.node)     — 把 B 的 node 从延时链表摘下来（改 4 个指针）
  2. task_add_to_ready(&TCB_B)    — 把 B 的 node 挂到就绪链表尾部（又改 4 个指针）

TCB_B 这块内存从头到尾没动过。纯粹是逻辑上的重新排队。
```

**哨兵节点 (Sentinel)**：

```
空链表:
  ┌──────────┐
  │ sentinel │
  │ next ────┼──→ 自己
  │ prev ────┼──→ 自己
  └──────────┘

包含 2 个节点:
  ┌──────────┐     ┌──────────┐     ┌──────────┐
  │ sentinel │←───→│  Node A  │←───→│  Node B  │
  └──────────┘     └──────────┘     └──────────┘
       ↑                                  │
       └──────────────────────────────────┘
                 (首尾相连，循环)
```

哨兵节点的作用是消除边界条件。插入第一个节点和插入第 N 个节点的代码完全相同——不需要 `if (list->head == NULL)` 这样的特殊判断。

**问：链表是循环的，遍历到 sentinel 不就死循环了吗？**

确实是个圈，但遍历时有明确的终止条件：

```c
// 遍历就绪链表 — 遇到 sentinel 就停
// 宏展开后: for (n = sentinel.next;  n != &sentinel;  n = n->next)
list_for_each(n, &ready_lists[prio]) {
    tcb_t *task = list_entry(n, tcb_t, node);
    // 处理 task...
}
```

流程是：`sentinel → NodeA → NodeB → sentinel（看到你了，停，不进去）`

sentinel 同时扮演两个角色：
- **起点**：`sentinel.next` 就是第一个数据节点
- **终点标志**：遍历到 `&sentinel` 时退出循环，不进它的 next

圈是真的圈，只是走路时看到哨兵就停。

**关键宏：从节点反查容器**

```c
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

// 用法：已知 node 指针，反查它所属的 tcb
list_node_t *node = list_first(&ready_lists[3]);
tcb_t *task = list_entry(node, tcb_t, node);
//           等价于: (tcb_t *)((char *)node - offsetof(tcb_t, node))
```

这利用了一个简单的事实：如果知道 `node` 成员在 `tcb_t` 中的偏移量，用 `node` 的地址减去偏移量，就得到了 `tcb_t` 的地址。

**container_of 逐层拆解：**

```c
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```

| 步骤 | 代码 | 作用 |
|------|------|------|
| 1 | `offsetof(type, member)` | 编译器内建宏，算出 member 在 type 里的字节偏移量。比如 `offsetof(tcb_t, node)` = 40 |
| 2 | `(char *)(ptr)` | 把指针强转成 `char*`，这样加减运算的单位是**字节**（否则 `ptr - 40` 会以为是减 40 个 `tcb_t`，实际会减 `40 × sizeof(tcb_t)` 字节） |
| 3 | `(char *)(ptr) - offset` | 用 node 地址减去偏移量，得到 tcb 的起始地址 |
| 4 | `(type *)...` | 把得到的地址强转回 `tcb_t*` 类型 |

图示：

```
  tcb_t 起始地址 (0x100000)              node 成员地址 (0x100040)
  ├──────────────────────────────────────────┤
  │ name[16] │ priority │ ... │ node.next/prev │
  └──────────────────────────────────────────┘
              offsetof(tcb_t, node) = 40 字节
              ├──────────────────────────────→|

  tcb 起始地址 = node地址(0x100040) - 偏移(40) = 0x100000 ✓
```

为什么要 `(char *)`？因为 C 的指针减法是按类型大小算的。`int *p = 0x100040; p - 40;` 会减掉 `40 × sizeof(int) = 160` 字节，不是 40 字节。转成 `char *`（大小为 1 字节）才能精确做字节减法。

### 3.3 学习检查点

在继续之前，确保你能回答：
1. `list_remove(node)` 为什么不需要传入链表？
2. 哨兵节点解决了什么问题？
3. 给定 `list_node_t *n`，如何找到包含它的 `tcb_t`？

运行验证：
```bash
make clean && make          # 应该零警告编译通过
```

---

## 4. Phase 2：任务控制块与任务创建

### 推荐先读这些文件

> `kernel.h` (TCB 结构体) → `task.h` → `task.c` → `kernel.c` (初始化部分)

### 4.1 任务控制块 (TCB)

[`kernel.h`](kernel.h) 中的 `tcb_t` 是整个 RTOS 最核心的数据结构：

```c
typedef struct tcb {
    /* --- 上下文信息 --- */
    ucontext_t      context;        // CPU 寄存器快照 (RIP, RSP, RBP...)
    uint8_t        *stack;          // 任务专属栈的起始地址
    uint32_t        stack_size;     // 栈大小 (字节)

    /* --- 任务属性 --- */
    uint8_t         priority;       // 当前优先级 (0 最高)
    uint8_t         base_priority;  // 基础优先级 (预留给互斥量)
    uint8_t         state;          // READY / RUNNING / BLOCKED
    char            name[16];       // 调试用任务名

    /* --- 延时信息 --- */
    uint32_t        delay_ticks;    // 还需等待的 tick 数

    /* --- 链表节点 (嵌入!) --- */
    list_node_t     node;           // 挂在 ready_lists[] 或 delayed_list
} tcb_t;
```

**每个任务拥有独立栈**——这是 RTOS 与裸机的本质区别：

```
裸机 (单栈):                       RTOS (多栈):
┌──────────────┐                   ┌──────────────┐
│    栈        │                   │ main 栈      │ (启动时用，被丢弃)
│  (所有变量、 │                   ├──────────────┤
│   调用帧共享)│                   │ Task A 栈    │ ← 切换时 RSP 指向这里
└──────────────┘                   ├──────────────┤
                                   │ Task B 栈    │ ← 切换时 RSP 指向这里
                                   ├──────────────┤
                                   │ idle 栈      │
                                   └──────────────┘
```

### 4.2 任务创建流程

[`task_create()`](task.c) 做了 4 件事：

```c
tcb_t *task_create(task_func, name, stack_size, priority) {
    // ① 分配 TCB
    task = malloc(sizeof(tcb_t));
    memset(task, 0, sizeof(tcb_t));

    // ② 分配独立栈
    task->stack = malloc(stack_size);

    // ③ 初始化 ucontext (设置栈和入口函数) — Phase 3 详解
    port_init_task_context(task, task_func);

    // ④ 加入就绪链表
    task_add_to_ready(task);  // 挂到 ready_lists[priority]
}
```

**为什么是这 4 步，顺序能换吗？**

每一步都有必须的理由，而且顺序严格不可颠倒：

| 步骤 | 做什么 | 为什么必须这一步 | 为什么必须这个顺序 |
|------|--------|-----------------|-------------------|
| ① 分配 TCB | `malloc(sizeof(tcb_t))` | 内核需要一个地方保存任务的所有元数据（优先级、状态、延时计数、链表节点）。没有 TCB，调度器没有任何东西可以管理。`memset` 清零防止未初始化字段（如 `delay_ticks`）有垃圾值 | 必须先有 TCB，才能往里面填信息 |
| ② 分配独立栈 | `malloc(stack_size)` | 每个任务必须有独立的调用栈，否则任务 B 运行时会覆盖任务 A 的栈帧，切回 A 时数据就烂了 | 必须在 ucontext 初始化之前，因为下一步需要把栈地址写入 context |
| ③ init ucontext | `port_init_task_context()` | 回答两个关键问题：这个任务从哪开始跑？（设置 RIP → 入口函数）用哪个栈？（设置 RSP → 独立栈）| 栈地址在手上才能设置 context |
| ④ 加入就绪链表 | `task_add_to_ready()` | 让任务对调度器"可见"。在此之前任务虽然准备好了，但不在 `ready_lists[]` 里，`find_highest_priority_task()` 扫链表时根本看不到 | 必须在所有信息就绪之后才能暴露给调度器 |

**为什么顺序不能乱**：如果先 add to ready 再 init ucontext，调度器可能在 context 还没设置好时就选中这个任务——RIP 是垃圾值，CPU 跳到随机地址，当场崩溃。

**关于 malloc**：`malloc` 是向操作系统"申请一块内存"，返回的是那块内存的**起始地址**，不是大小。

```c
tcb_t *task = malloc(sizeof(tcb_t));
//  task 是个指针，存的是地址（如 0x100000），不是 sizeof(tcb_t) 的值（120）
//  大小信息在堆管理器内部记录着，方便 free 时知道要释放多少
```

用 `malloc` 而不是直接定义变量的原因：任务是**运行时动态创建**的——写代码时不知道要建几个任务、每个多大栈。`malloc` 在堆上分配，内存的生命周期不受函数调用栈限制，不会因为 `task_create` 返回就失效。
```

### 4.3 就绪列表的组织

```c
list_t ready_lists[MAX_PRIORITIES];  // 定义在 kernel.c

// ready_lists[0]: 优先级 0 的任务 (最高)  →  [task_high]
// ready_lists[1]: 优先级 1 的任务         →  [task_a] → [task_b]
// ready_lists[2]: 优先级 2 的任务         →  (空)
// ...
// ready_lists[7]: 优先级 7 的任务 (idle)   →  [idle]
```

**为什么每个优先级一个链表？** 查找最高优先级任务只需从 0 开始扫描，找到第一个非空链表即可。FreeRTOS 进一步用位图 (`uxTopReadyPriority`) 加速到 O(1)。

### 4.4 空闲任务 (Idle Task)

```c
// kernel.c - kernel_init() 中创建
idle_task_tcb.priority = IDLE_PRIORITY;  // 7，最低
port_init_task_context(&idle_task_tcb, idle_task_func);
task_add_to_ready(&idle_task_tcb);
```

空闲任务是 RTOS 的"安全网"——确保 CPU 永远不会没有任务可运行。在真实系统中，空闲任务调用 `__WFI()` 进入低功耗模式。

**深入理解：IDLE_PRIORITY 到底起了什么作用？**

`IDLE_PRIORITY = 7` 的作用体现在调度器的优先级查找机制 `find_highest_priority_task()` 中：

```c
// kernel.c - 从 prio=0（最高）开始扫描
for (int prio = 0; prio < MAX_PRIORITIES; prio++) {
    if (!list_is_empty(&ready_lists[prio])) {  // 找到第一个非空链表
        return list_entry(list_first(&ready_lists[prio]), tcb_t, node);
    }
}
```

空闲任务被钉在最低优先级（7），只要 0~6 级上有任何任务就绪，空闲任务就绝不可能被选中。只有当所有用户任务（如 Task High prio=2, Task Low prio=4）都在延时阻塞状态时，调度器才会一路扫描到 `ready_lists[7]`，选中空闲任务。

一句话：**永远把空闲任务标记成最低优先级，只有当没有更要紧的任务时才会执行它。**

### 4.5 学习检查点

运行验证：
```bash
make clean && make && ./build/lite_rtos_demo
```
应该看到：
```
=== Ready List Status ===
Priority 1: [sensor]
Priority 3: [led1] [led2]
Priority 7: [idle]
```

---

## 5. Phase 3：上下文切换的原理与实现

### 推荐先读这些文件

> `port.h` → `port.c` (这是整个内核最"魔法"的部分)

### 5.1 什么是上下文切换？

上下文切换就是：**保存当前任务的所有 CPU 寄存器 → 加载另一个任务的寄存器**。

```
切换前:  CPU 正在执行 Task A
         RSP → Task A 的栈
         RIP → Task A 的代码

切换中:  保存: RSP, RIP, RBP, R8-R15... → Task A 的 context
        加载: Task B 的 context → RSP, RIP, RBP, R8-R15...

切换后:  CPU 正在执行 Task B
         RSP → Task B 的栈
         RIP → Task B 的代码 (从上次停下的地方继续)
```

**从 Task A 的视角看**：调用了一个"非常慢的函数"(`swapcontext`)，等它返回时，自己又继续执行了——完全不知道中间 Task B 运行过。

**形象比喻：ucontext_t 就是每个任务的"记事本"**

`ucontext_t` 是 CPU 在某一个瞬间的完整寄存器快照。就像每个任务都有一个记事本，记录着"我的工作做到哪里了"：

| 记事本上记什么 | 对应寄存器 | 具体含义 |
|---|---|---|
| "做到第几行了" | **RIP** | 当前执行到代码的哪条指令 |
| "用哪张草稿纸" | **RSP / RBP** | 当前用哪个栈、栈用到了哪 |
| "算出来的中间结果" | **RAX, RBX, RCX...** | 函数返回值、循环计数器、临时计算结果 |

`swapcontext()` 做的事就是：

```
1. Task A 把当前手里的活全部记到自己记事本上（保存所有寄存器到 A.context）
2. 翻开 Task B 的记事本，照着恢复：做到哪了、草稿纸上有什么、中间结果是多少（从 B.context 恢复所有寄存器）
3. CPU 直接从 Task B 上次停下的地方接着跑
```

注意：恢复的寄存器里包含 RSP（栈指针），所以"切换栈"只是"切换所有寄存器"的其中一步——恢复 B.context 时 RSP 自然就指向 B 的栈了。

### 5.2 ucontext 机制详解

在 ARM 硬件上，上下文切换用汇编操作 PendSV 异常实现。在 PC 上，我们用 POSIX 的 `ucontext.h` 模拟。

**`getcontext`**：获取当前 CPU 寄存器状态，保存到 `ucontext_t` 中。

**`makecontext`**：修改 `ucontext_t`，设置入口函数和栈指针。调用后，当这个 context 被激活时，CPU 会从指定的函数开始执行。

**`swapcontext(from, to)`**：等价于 `getcontext(from)` + `setcontext(to)`。保存当前状态到 `from`，恢复 `to` 的状态。

**`setcontext(ctx)`**：跳转到 `ctx` 保存的状态，**不保存**当前状态（"一去不回"，单程票）。

**`setcontext` vs `swapcontext` 的本质区别**：

| | `setcontext(ctx)` | `swapcontext(from, to)` |
|---|---|---|
| 保存当前位置 | 不保存 | 保存到 `from->context` |
| 跳到目标 | 跳到 `ctx` | 跳到 `to->context` |
| 以后能回来吗 | 不能，单程票 | 能，别人切回 from 就从调用处之后继续 |
| 用在哪 | `kernel_start` 首次启动，main 上下文直接丢弃 | 任务之间互相切换 |

所以启动时用 `setcontext`（main 不需要被保存），之后任务间切换用 `swapcontext`（A 和 B 互相保存对方的进度）。

**关于字段命名 `uc_stack.ss_size`**：

```c
task->context.uc_stack.ss_size
//       ↑        ↑       ↑
//   ucontext_t  栈配置   栈大小
//   (uc =       (ss =   (size =
//   ucontext)  stack)  stack size)
```

这些字段来自 POSIX 标准定义在 `<ucontext.h>` 中：

```c
typedef struct ucontext_t {
    ucontext_t *uc_link;       // uc = ucontext 缩写
    sigset_t    uc_sigmask;    // 信号掩码
    stack_t     uc_stack;      // 栈信息
    mcontext_t  uc_mcontext;   // 机器上下文（寄存器值）
    ...
} ucontext_t;

typedef struct {
    void  *ss_sp;       // ss = stack, sp = stack pointer（栈起始地址）
    size_t ss_size;     // ss = stack, size = 栈字节大小
    int    ss_flags;    // 标志位
} stack_t;
```

拆开读就是：`task->context`（任务的 CPU 快照）的 `.uc_stack`（栈配置）的 `.ss_size`（大小）。

### 5.3 代码流程

```c
// ① 创建任务时：设置 "第一次运行的现场"
void port_init_task_context(tcb_t *task, void (*task_func)(void)) {
    getcontext(&task->context);                    // 获取当前 CPU 状态作为模板
    task->context.uc_stack.ss_sp = task->stack;     // 替换栈指针：指向任务专属栈
    task->context.uc_stack.ss_size = task->stack_size;
    makecontext(&task->context, task_func, 0);      // 设置入口函数
}
// 当这个 context 首次被激活时，CPU 会：
//   1. 切换到 task->stack 指向的栈
//   2. 跳转到 task_func 开始执行

// ② 启动调度器时：跳转到第一个任务
void port_start_first_task(tcb_t *task) {
    setcontext(&task->context);  // 跳转，不保存当前 context
    // 永远不会返回到这里
}

// ③ 任务切换时：保存当前、恢复下一个
void port_switch_context(tcb_t *from, tcb_t *to) {
    swapcontext(&from->context, &to->context);
    // 当 from 任务再次被调度时，从这里继续执行
}
```

### 5.4 为什么需要独立栈？

考虑以下场景：
```
Task A 运行到一半，被 Task B 抢占。
如果 Task A 和 Task B 共用同一个栈：
  Task A 的局部变量在栈上
  Task B 运行时会覆盖 Task A 的栈帧
  Task B 返回后 → Task A 继续 → 栈上的数据已经被破坏了！💥
```

每个任务有独立栈 = 每个任务的调用帧互不干扰。

**形象比喻：车间 + 工位 + 工作台卡**

把整个 RTOS 系统想象成一个只有**一个工人（CPU 单核）**的车间：

```
车间里：
  ┌─────────────────────────────────────────────────────────┐
  │                                                         │
  │   [工位]  ← 工人一次只能坐一个工位干活                    │
  │                                                         │
  │   工位 A          工位 B          工位 C                 │
  │  ┌─────────┐    ┌─────────┐    ┌─────────┐              │
  │  │ 图纸翻到  │    │ 图纸翻到  │    │ 图纸翻到  │             │
  │  │ 第3页    │    │ 第7页    │    │ 第1页    │             │
  │  │          │    │          │    │          │              │
  │  │ 草稿纸上  │    │ 草稿纸上  │    │ 草稿纸上  │             │
  │  │ 结果=42  │    │ 结果=99  │    │ 结果=0   │             │
  │  │          │    │          │    │          │              │
  │  │ 待办便签  │    │ 待办便签  │    │          │              │
  │  │ "等零件"  │    │ "继续算"  │    │          │              │
  │  └─────────┘    └─────────┘    └─────────┘              │
  │                                                         │
  └─────────────────────────────────────────────────────────┘
```

| 车间概念 | RTOS 对应 | 说明 |
|---------|----------|------|
| 工位 | 任务的栈（那块 malloc 内存） | 一个任务独占一个工位，工位之间互不干扰 |
| 图纸翻到第几页 | RIP | 代码执行到哪条指令了 |
| 草稿纸上的结果 | RAX, RBX 等通用寄存器 | 中间计算结果 |
| 等待时间的留言 | delay_ticks | 被告知要等待多久 |
| 待办便签 | state（READY/BLOCKED/RUNNING） | 任务状态 |
| 工作台卡 | TCB | 记录这个工位的一切信息 |
| 工人 | CPU 单核 | 同一时刻只能坐一个工位 |
| 车间主任 | 调度器 scheduler | 决定让工人换到哪个工位干活 |

**调度过程：**

```
1. 工人在工位 A 干活 → Task A RUNNING
2. Task A 说"我在等零件来（延时）" → task_delay(10)
3. 车间主任喊：停！把当前进度记在工作台卡 A 上 → 保存所有寄存器到 A.context
4. 工人走到工位 B，翻开工作台卡 B → 恢复 B.context
5. 工人照着 B 的图纸和草稿纸接着干 → Task B RUNNING
6. 等 A 的零件到了（10 个 tick 到了）→ A 从延时链表回到就绪链表（READY）
7. 车间主任觉得 B 该暂停了 → 保存 B 的进度到 B.context
8. 工人回到工位 A，翻开工作台卡 A，接着第三步的进度继续 → Task A RUNNING
```

**每个工位的桌面（栈）结构：**

```
工位 A 的桌面（栈，8KB 内存块）：
  高地址 ┌────────────────┐
        │ funcA 的局部变量  │  ← 调用 funcA 时压的
        │ funcB 的局部变量  │  ← funcA 调了 funcB，又压一层
        │ 返回地址         │  ← funcB 执行完该回到哪
  RSP → │ 当前函数局部变量  │  ← 现在在用这部分
  低地址 └────────────────┘

工位 B 的桌面（另一块独立的 8KB 内存块）：
  高地址 ┌────────────────┐
        │ funcX 的局部变量  │
  RSP → │ funcY 的局部变量  │
  低地址 └────────────────┘
```

核心就是：**工人只有一个，但每个工位都有自己完整的一套资料。切换工位 = 保存当前桌面状态 + 恢复另一个桌面状态。工位之间互不干扰。**

这也是为什么"一个栈 = 一个任务，严格一对一，不能共享"——如果两个任务共用一个栈，相当于把东西写在同一个桌面上的同一个位置，前后覆盖，数据就烂了。

### 5.5 学习检查点

运行验证：
```bash
make clean && make && ./build/lite_rtos_demo
```
应该看到 Task A 和 Task B 交替打印。"魔法"的证明：两个 `while(1)` 循环在同一个 CPU 上交替执行！

---

## 6. Phase 4：优先级调度器

### 推荐先读这些文件

> `kernel.c` (schedule, find_highest_priority_task, kernel_start, task_yield)

### 6.1 调度算法

```c
// 核心问题：有 N 个就绪任务，该选谁运行？
static tcb_t *find_highest_priority_task(void) {
    for (int prio = 0; prio < MAX_PRIORITIES; prio++) {  // 从最高优先级开始
        if (!list_is_empty(&ready_lists[prio])) {
            return list_entry(list_first(&ready_lists[prio]), tcb_t, node);
        }
    }
    return NULL;  // 不应发生：至少 idle 任务始终就绪
}
```

**时间复杂度**：O(MAX_PRIORITIES) = O(8) ≈ O(1)。FreeRTOS 用位图进一步优化到真正的 O(1)。

### 6.2 schedule() 函数

这是内核的心脏：

```c
void schedule(void) {
    port_disable_interrupts();        // 临界区：禁止中断打断链表操作

    // ① 处理 tick（Phase 5 详解）
    if (port_tick_pending()) { process_tick(); }

    // ② 选择最高优先级的就绪任务
    tcb_t *next = find_highest_priority_task();

    // ③ 如果和当前任务不同，切换
    if (next != current_task) {
        tcb_t *prev = current_task;
        prev->state = TASK_READY;
        next->state = TASK_RUNNING;
        current_task = next;

        port_enable_interrupts();
        port_switch_context(prev, next);  // ← 上下文切换发生在这里！
        // ─────────────────────────────────────
        // prev 下次被调度时从这里继续
        // ─────────────────────────────────────
    } else {
        port_enable_interrupts();
    }
}
```

### 6.3 task_yield() — 主动让出 CPU

```c
void task_yield(void) {
    task_remove_from_ready(current_task);  // 从就绪链表移除
    task_add_to_ready(current_task);       // 加到就绪链表尾部
    schedule();                            // 触发调度
}
```

把自己移到链表尾部 → 同优先级的其他任务有机会运行 → FIFO 公平性。

**task_yield() vs task_delay() 的本质区别**：

| | `task_yield()` | `task_delay(N)` |
|---|---|---|
| 任务去哪里 | 留在就绪链表尾部 | 移到延时链表（阻塞态 BLOCKED） |
| 下次什么时候跑 | 同优先级任务轮完一圈就轮回来 | 等 N 个 tick 到期才唤醒 |
| 用途 | **同优先级时间片轮转** | 延时等待（等数据、等外设、周期性任务） |
| 状态转换 | RUNNING → READY（还在就绪链表） | RUNNING → BLOCKED → tick到期 → READY |

关键区别：`task_delay()` 是从就绪链表**摘走**，放到延时链表，等倒计时到零才回来。`task_yield()` 只是把自己**挪到**就绪链表尾部，马上就能被再次调度，只是让同优先级任务先跑一轮。

### 6.4 第一个任务如何启动

```c
void kernel_start(void) {
    scheduler_running = 1;

    tcb_t *first = find_highest_priority_task();  // 选最高优先级
    current_task = first;
    first->state = TASK_RUNNING;

    port_start_first_task(first);  // 跳转，永不返回
    // main() 的执行上下文到此被"丢弃"
}
```

### 6.5 学习检查点

运行 demo，观察输出：
```
[Task A] iteration 0    ← kernel_start 选择 task_a
[Task B] iteration 0    ← task_a yield → schedule() 选 task_b
[Task A] iteration 1    ← task_b yield → schedule() 选 task_a
[Task B] iteration 1
...
```
两个任务严格交替，证明调度器正确工作。

---

## 7. Phase 5：系统节拍与任务延时

### 推荐先读这些文件

> `port.c` (tick 部分) → `kernel.c` (process_tick) → `task.c` (task_delay)

### 7.1 为什么需要系统节拍？

RTOS 需要感知**时间**。`task_delay(100)` 意味着"把我阻塞 100 个 tick，之后唤醒我"。

在真实硬件上，这是 SysTick 定时器中断实现的。在 PC 上：
```
setitimer(ITIMER_REAL)  →  每 10ms 触发一次 SIGALRM
                                ↓
                        tick_signal_handler()
                                ↓
                        设置 tick_pending = 1
                                ↓
                        schedule() 中处理
```

### 7.2 Tick 频率的选择

```c
#define TICK_RATE_HZ     100          // 100 Hz
#define TICK_INTERVAL_US (1000000/100) // = 10000μs = 10ms
```

- **太高** (如 1000Hz)：CPU 频繁处理中断，开销大
- **太低** (如 10Hz)：任务延时精度差
- **100Hz**：10ms 精度，适合大多数嵌入式应用

### 7.3 延时链表的处理

```c
// kernel.c - 在 schedule() 中调用
static void process_tick(void) {
    tick_count++;

    // 遍历延时链表，递减每个任务的 delay_ticks
    list_for_each_safe(node, tmp, &delayed_list) {
        tcb_t *task = list_entry(node, tcb_t, node);
        task->delay_ticks--;

        if (task->delay_ticks == 0) {
            // 延时到期！从延时链表移除，加入就绪链表
            list_remove(node);
            task_add_to_ready(task);   // 状态变为 READY
        }
    }
    port_clear_tick();   // ← 这一行做了什么？
}
```

**`port_clear_tick()` 有什么用？**

实现就一句话：`tick_pending = 0;` —— 把 tick 标记擦掉。

```
SIGALRM 信号到来 → tick_pending = 1;  // "有人敲门"

schedule() → process_tick()
  ├─ tick_count++
  ├─ 遍历延迟链表，递减 delay_ticks，到期的搬回就绪链表
  └─ port_clear_tick();  // tick_pending = 0  "处理完了，擦掉标记"
```

如果不擦掉，下次 `schedule()` 被调用时会再执行一遍 `process_tick()`，重复递减 `delay_ticks`，导致延时还没到就被错误唤醒。必须"处理完就清"，保证每个 tick 只处理一次。

**每个 tick 结束后 schedule() 都要跑一遍吗？**

**不是。** `tick_pending` 是布尔标志，不是计数器。schedule() 只在三个地方被调用：

| 调用位置 | 触发时机 |
|---------|---------|
| `task_yield()` | 任务主动让出 CPU |
| `task_delay()` | 任务延时阻塞 |
| `idle_task_func()` | 空闲任务发现有 tick 待处理 |

如果某个任务一直干活不 yield 不 delay（比如一个死循环算东西），schedule() 可能好几个 tick 都不跑一次。所有 tick 信号都被"压缩"成 `tick_pending = 1` 这一个标志位——布尔标志只能表示"有没有"，不能表示"有几个"。

**那延时的 tick 计数不就错了吗？**

理论上确实会丢 tick：多个 tick 到来，`tick_pending` 始终是 1，`schedule()` 只处理一次，`process_tick()` 只减一次 `delay_ticks`。

但在这个项目的 demo 中实际不会出错，因为任务延时很短——Task Low 每 3 个 tick 调一次 `task_delay()`，Task High 每 10 个 tick 调一次，`schedule()` 频繁被调用，tick 来不及堆积。

真正会出问题的场景是：有个任务长时间不调 yield/delay（如 `compute_10_seconds()`），那期间的所有 tick 全部丢失，延时链表上的任务得不到精确唤醒。这就是 §8.3 要讨论的 PC 模拟与真实硬件的差距。

**"tick 到期"到底是什么意思？**

所谓"tick 到期"就是 **`delay_ticks` 从 N 递减到 0 的那一刻**，像一个倒计时闹钟：

```
task_delay(5) → delay_ticks = 5
tick 1: delay_ticks 减到 4
tick 2: delay_ticks 减到 3
tick 3: delay_ticks 减到 2
tick 4: delay_ticks 减到 1
tick 5: delay_ticks 减到 0 → "到期"！摘下来放回就绪链表
```

每个任务在 TCB 里都有独立的 `delay_ticks` 字段，各个任务各自倒计时，互不影响。

**为什么 tick 到期后是 READY 而不是 RUNNING？**

tick 到期只是取消了任务的"阻塞"状态，把它放回就绪链表。但能不能立刻跑，还要看调度器：

- 如果它的优先级**最高** → `schedule()` 选中它，READY → RUNNING
- 如果还有**更高优先级**的任务在就绪态 → 它留在 READY，排队等着

所以状态转换是 `BLOCKED → tick到期 → READY → scheduler选中 → RUNNING`，分开的。

**每个任务的延时长短可以各自设置吗？**

可以，每个任务调 `task_delay(N)` 时各自传不同的 `N`，互不影响。例如 demo.c 里 Task High 等 10 个 tick，Task Low 等 3 个 tick。`process_tick()` 逐个递减，各减各的。

**task_delay() 模拟的是什么？**

它模拟的是"任务在等待某个条件，暂时没事干"——等外设数据、等硬件操作完成、等按键去抖、或者纯粹是周期性任务（每 100ms 采集一次）。关键是不做忙等（busy loop），而是把 CPU 让出来给其他任务用。

**时间复杂度**：O(N_delayed)，每次 tick 遍历所有延时任务。FreeRTOS 将延时链表按到期时间**排序**，只需检查链表头部的任务，优化到 O(1) 摊销。

### 7.4 task_delay() 的实现

```c
void task_delay(uint32_t ticks) {
    port_disable_interrupts();          // 临界区

    task_remove_from_ready(current_task);                 // ① 离开就绪列表
    current_task->state = TASK_BLOCKED;                   // ② 标记为阻塞
    current_task->delay_ticks = ticks;                    // ③ 设置倒计时
    list_insert_tail(&delayed_list, &current_task->node); // ④ 加入延时列表

    port_enable_interrupts();
    schedule();                          // ⑤ 触发调度 (切换到其他任务)
    // ──────────────────────────────────────
    // ticks 个系统节拍后，会被 schedule() 唤醒
    // ──────────────────────────────────────
}
```

**执行流程图**：

```
task_delay(5) 调用:
  ┌─────────────────────────────────────────────────────────┐
  │ Task A 从 ready_lists[1] 移除                           │
  │ Task A.state = BLOCKED                                  │
  │ Task A.delay_ticks = 5                                  │
  │ Task A 加入 delayed_list                                │
  │ schedule() → 选 Task B → swapcontext(A, B)              │
  ├─────────────────────────────────────────────────────────┤
  │ ... Task B 运行 ...                                     │
  │ ... 每个 tick: A.delay_ticks 递减: 5→4→3→2→1→0         │
  │ ... tick 5: A.delay_ticks == 0 → 从 delayed_list 移除   │
  │               → task_add_to_ready(A) → A.state = READY  │
  │ ... schedule() 被调用 → A 是最高优先级 → swapcontext(B, A) │
  ├─────────────────────────────────────────────────────────┤
  │ Task A 从 task_delay() 的 schedule() 调用返回，继续执行  │
  └─────────────────────────────────────────────────────────┘
```

### 7.5 信号安全：为什么 tick handler 只设置标志？（Phase 5/6 设计）

```c
// Phase 5/6 的 tick handler: 最安全但最保守的做法
static void tick_signal_handler(int sig) {
    tick_pending = 1;   // 仅此而已！
}
```

`SIGALRM` 可能在**任何时刻**触发——包括正在 `list_insert_tail()` 的中间。如果 handler 中直接操作链表，会导致数据损坏。因此 handler 只设置 `sig_atomic_t` 标志，实际的链表操作推迟到 `schedule()`（中断被阻塞时）进行。

> **进阶**：Phase 7（真抢占）突破了这一限制——信号处理函数直接调用 `schedule_from_isr()` 完成调度。这是通过严格的异步信号安全审计和 `sigprocmask` 管理实现的。详见 §9.5。

**如果硬要在信号处理函数里操作链表会怎样？**

假设 Task A 正在往就绪链表插节点，`list_insert_tail()` 执行到一半：

```c
// list.c - list_insert_tail()，四个步骤
node->next = &list->sentinel;        // ① 刚执行完
//          ↑ SIGALRM 信号在这里到来！CPU 跳到信号处理函数
node->prev = list->sentinel.prev;    // ② 还没来得及执行
list->sentinel.prev->next = node;    // ③
list->sentinel.prev = node;          // ④
```

此时链表是**半成品**：`node->next` 指向 sentinel，但 sentinel 还没指向 node，其他节点的指针也悬着。

```
链表现在长这样（已经烂了）：
  sentinel ←?→ [Node X]     [新 node 半挂着]
       ↑ next 指什么？      ↑ prev 还是垃圾值/旧值
```

如果信号处理函数此时也去操作链表（遍历、删除、插入），读到这个半成品——要么指针飞到非法地址**崩溃**，要么**静默地操作了错误节点**，产生更难排查的 bug。

这种并发问题叫 **race condition**：信号处理函数可能在任意一条机器指令之间插入执行，完全不可预测。可能 99.9% 的时间不出问题，第 1000 次运行才崩。

### 7.6 学习检查点

观察 Phase 5 demo 输出：
```
[Task B] tick=3, count=1     ← B delay(3), 精确在 tick 3 唤醒
[Task A] tick=5, count=1     ← A delay(5), 精确在 tick 5 唤醒
[Task B] tick=6, count=2     ← 3+3=6 ✓
[Task B] tick=9, count=3     ← 6+3=9 ✓
[Task A] tick=10, count=2    ← 5+5=10 ✓
```

验证延时时间精确匹配。

---

## 8. Phase 6：抢占式调度（延迟抢占）

### 8.1 合作式 vs 抢占式

```
合作式调度 (Phase 1-5):
  任务必须主动 task_yield() 或 task_delay()，其他任务才能运行。
  如果某个任务写了 while(1) {} 而不 yield，整个系统卡死。

延迟抢占式调度 (Phase 6):
  高优先级任务就绪时，内核在下一个"调度点"暂停当前任务。
  调度点 = task_delay() / task_yield() / idle 轮询。
  如果任务从不调用这些 API，高优先级任务仍然无法运行。

真抢占式调度 (Phase 7):
  高优先级任务就绪时，ISR 直接在中断上下文中完成上下文切换。
  不管当前任务在做什么（死循环、计算密集型代码），
  都会被立即强制暂停。详见下一章。
```

### 8.2 抢占如何发生

```
时间线:
tick 0:  HIGH 运行(prio=0) → delay(10)
         LOW  运行(prio=3) → delay(3)

tick 3:  LOW 运行 → delay(3)
tick 6:  LOW 运行 → delay(3)
tick 9:  LOW 运行 → delay(3)

tick 10: SIGALRM → tick_pending = 1
         LOW 在 tick 9 调用了 delay(3)，进入 schedule()
         schedule() → process_tick() → HIGH delay_ticks 归零
         → task_add_to_ready(HIGH) — 优先级 0
         → find_highest_priority_task() → HIGH!
         → swapcontext(LOW, HIGH) ← 抢占！
         HIGH 运行 → delay(10)

tick 12: LOW 运行 → delay(3)
...
```

**抢占的关键**：当 `schedule()` 被调用时（通过 task_delay），它发现 `ready_lists[0]` 中有任务，优先级高于当前任务（prio 3），于是执行切换——HIGH 抢占了 LOW。

### 8.3 PC 模拟 vs 真实硬件

| 特性 | PC 模拟 | ARM Cortex-M |
|------|---------|--------------|
| 中断源 | SIGALRM 信号 | SysTick 硬件定时器 |
| 上下文保存 | ucontext (OS 自动) | 硬件自动压栈 R0-R3,R12,LR,PC,xPSR |
| 上下文切换 | swapcontext() | PendSV 汇编：手动保存 R4-R11，切换栈指针 |
| 抢占时机 | Phase 6: 内核 API 调用边界<br>Phase 7: ISR 中直接切换 (见 §9) | SysTick ISR 返回时立即触发 PendSV |
| 临界区 | 阻塞 SIGALRM | 禁用中断 (BASEPRI / PRIMASK) |

**为什么 PC 上延迟抢占曾被限制在 API 边界？** 因为信号 handler 中操作内核数据结构不安全（不是异步信号安全的）。Phase 7 通过严格的异步信号安全审计（只做指针/整数操作 + swapcontext）和 sigprocmask 管理突破了这一限制。真实硬件没有这个问题——中断 handler 中可以直接操作就绪链表，因为硬件保证中断上下文不会被其他中断打断（同优先级或更低优先级的中断被屏蔽）。

**ARM 硬件为什么可以安全操作？四层保护机制：**

**第一层：硬件自动关中断。** ARM 上进入 SysTick ISR 的瞬间，CPU 硬件自动屏蔽同优先级和更低优先级的所有中断。不会有第二个中断来打断你的 ISR。退出 ISR 时硬件自动恢复。整个过程不需要软件干预。

对比 PC：SIGALRM 信号随时可能到来，没有任何硬件保护，可能打断任何一条机器指令，包括 `list_insert_tail` 写到一半。

**第二层：硬件自动保存一半寄存器。** ARM 进中断时，CPU **硬件自动压栈** R0-R3, R12, LR, PC, xPSR 这 8 个寄存器到当前任务的栈上。不需要软件做任何事。退出中断时硬件自动弹出恢复。

对比 PC：信号来了，操作系统在内核态手动保存全部寄存器，再切回用户态跑 handler——代价大，而且用的是同一个栈。

**第三层：PendSV（可挂起系统服务）的分层设计。** ARM 上真正做上下文切换的不是 SysTick ISR，而是一个叫 PendSV 的异常：

```
优先级（数字越大越低）：
  高  SysTick ISR   → 只做 tick 计数 + 延时到期判断 + 触发 PendSV
  中  其他外设中断   → 正常处理
  低  PendSV        → 真正做上下文切换（改链表、切栈、swap）
```

PendSV 优先级设成**最低**，意味着它只在所有其他中断都处理完、CPU 准备返回任务时才会运行。绝对不会打断任何 ISR 的链表操作。FreeRTOS 的命名就是从这里来的——**Pen**ding **S**er**V**ice。

对比 PC：没有 PendSV 这种分层机制，SIGALRM 只有一个优先级，没法分层。

**第四层：关中断是一条指令。** ARM 上进入临界区只需一条汇编指令 `CPSID I`（1 个 CPU 周期），退出用 `CPSIE I`。极快，极可靠。

对比 PC：`sigprocmask()` 是一个系统调用，需要从用户态陷入内核态再返回，开销巨大，而且它本身就不是 async-signal-safe 的。

---

**一句话总结**：ARM 用"硬件自动关中断 + 自动压栈 + PendSV 分层 + 单指令临界区"四层保护，让 ISR 里操作数据结构既安全又高效。PC 的信号是操作系统软件层面硬塞进来的，"随时打断 + 没有分层 + 系统调用开销大"，只能妥协为"设标志，推迟处理"。

### 8.4 临界区保护

```c
void port_disable_interrupts(void) {
    sigaddset(&block_mask, SIGALRM);
    sigprocmask(SIG_BLOCK, &block_mask, NULL);  // 阻塞 SIGALRM
}

void port_enable_interrupts(void) {
    sigprocmask(SIG_UNBLOCK, &block_mask, NULL);  // 恢复
}
```

临界区 (Critical Section) 保护所有修改全局数据结构（就绪链表、延时链表）的代码路径。在 ARM 上，这通过 `__disable_irq()` / `__enable_irq()` 或操作 BASEPRI 寄存器实现。

### 8.5 学习检查点

Phase 6 demo 的关键观察：
```
⚡ [HIGH] tick=10, count=1 (PREEMPTED!)    ← HIGH 在 tick 10 唤醒，抢占 LOW
⚡ [HIGH] tick=20, count=2 (PREEMPTED!)    ← HIGH 每 10 tick 精确运行
⚡ [HIGH] tick=30, count=3 (PREEMPTED!)    ← tick 30 两者同时到期，HIGH 先运行
```

---

## 9. Phase 7：真抢占式调度（ISR 驱动）

### 推荐先读这些文件

> `port.c` (tick_signal_handler) → `kernel.c` (schedule_from_isr, schedule_select_and_switch) → `demo_preempt_true.c`

### 9.1 Phase 6 的局限：为什么延迟抢占不够？

回顾 Phase 6 的抢占机制：

```
tick 10: SIGALRM → tick_pending = 1 → 返回
         ... LOW 继续运行 ...
         LOW 调用 task_delay(3) → schedule()
           → process_tick() → HIGH 到期 → swapcontext(LOW, HIGH)
           ↑ 抢占发生在 task_delay() 内部！
```

**问题**：如果 LOW 是一个 CPU 密集型死循环——

```c
void task_low_func(void) {
    while (1) {
        compute_pi_decimal();  // 纯计算，从不调用 task_delay
    }
}
```

那么 `schedule()` 永远不会被调用，`tick_pending` 永远不会被处理，HIGH 任务永远不会运行。**整个系统卡死。**

### 9.2 真抢占的解决方案

**核心思路**：让信号处理函数直接完成调度，不等下次 `schedule()` 调用。

```
之前 (Phase 6, 延迟抢占):
  SIGALRM → tick_signal_handler()
              → tick_pending = 1;     // 只设标志
              → return;               // 返回原任务
  ... 等下次 task_delay/yield/idle 轮询 ...
  schedule() → process_tick() → swapcontext()

之后 (Phase 7, 真抢占):
  SIGALRM → tick_signal_handler()
              → tick_pending = 1;
              → schedule_from_isr();   // 直接调度！
                → process_tick()       // 处理 tick
                → sigprocmask(UNBLOCK) // 解阻塞信号
                → schedule_select_and_switch()
                  → find_highest_priority_task()
                  → swapcontext(prev, next)  ← 立即切换！
```

### 9.3 架构设计：三个函数的职责划分

真抢占重构引入了清晰的三层结构：

```
┌──────────────────────────────────────────────────────┐
│  调用层                                               │
│                                                      │
│  schedule()             schedule_from_isr()           │
│  任务上下文入口          ISR/信号上下文入口             │
│  (task_delay 调用)      (tick_signal_handler 调用)    │
│       │                      │                       │
│       ├─ disable interrupts   ├─ process_tick()      │
│       ├─ process_tick()       ├─ sigprocmask(UNBLOCK)│
│       ├─ enable interrupts    │         │             │
│       │        │              │         │             │
│       └────────┼──────────────┘         │             │
│                ▼                        │             │
│  schedule_select_and_switch()  ←────────┘             │
│  共享调度核心 (异步信号安全)                            │
│    ├─ find_highest_priority_task()                    │
│    └─ port_switch_context(prev, next)                 │
└──────────────────────────────────────────────────────┘
```

**`schedule_select_and_switch()`** — 纯调度逻辑，不碰信号掩码，不含 printf/malloc：

```c
static void schedule_select_and_switch(void)
{
    tcb_t *next = find_highest_priority_task();

    /* idle 任务始终就绪，next 不可能为 NULL */
    if (next == NULL || next == current_task) {
        return;
    }

    tcb_t *prev = current_task;

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    current_task = next;

    port_switch_context(prev, next);
    /* ---- 上下文切换发生在这里 ---- */
}
```

关键设计决策：
- 移除了 `fprintf(stderr, ...)` 错误处理 → 信号上下文中不安全
- 改为静默返回 → idle 任务始终就绪，NULL 场景不会发生
- 不调用 `sigprocmask` → 由调用者管理信号掩码

**`schedule_from_isr()`** — ISR 入口，管理信号掩码生命周期：

```c
void schedule_from_isr(void)
{
    /* 1. 处理 tick */
    process_tick();

#ifndef _WIN32
    /* 2. 解除 SIGALRM 阻塞
     *    当前在信号处理函数中，SIGALRM 被内核自动阻塞。
     *    必须在切换到新任务前解阻塞，否则新任务永远无法被抢占。 */
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGALRM);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
    }
#endif

    /* 3. 调度决策 + 上下文切换 */
    schedule_select_and_switch();
    /* 当被抢占任务恢复时，从此处继续 */

#ifndef _WIN32
    /* 4. 恢复信号掩码
     *    重新阻塞 SIGALRM 匹配信号处理函数上下文。
     *    返回后 sigreturn 恢复任务原始掩码。 */
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGALRM);
        sigprocmask(SIG_BLOCK, &mask, NULL);
    }
#endif
}
```

**`schedule()`** — 任务上下文入口（简化后）：

```c
void schedule(void)
{
    port_disable_interrupts();

    if (port_tick_pending()) {
        process_tick();
    }

    /* 先开中断再切换，确保新任务以中断开启状态运行 */
    port_enable_interrupts();
    schedule_select_and_switch();
}
```

### 9.4 信号掩码生命周期 — 这是正确性的关键

真抢占最精妙的部分是信号掩码管理。如果搞错，新任务可能带着被阻塞的 SIGALRM 运行，导致它永远无法再被抢占。

```
Task A 运行中 (SIGALRM unblocked)
  │
  │ SIGALRM 触发
  ▼
┌─ 内核自动阻塞 SIGALRM ──────────────────────────────┐
│                                                     │
│ tick_signal_handler()                               │
│   → tick_pending = 1;                               │
│   → schedule_from_isr()                             │
│       → process_tick()           // 唤醒到期任务      │
│       → sigprocmask(UNBLOCK)     // ❶ 解阻塞 SIGALRM │
│       → schedule_select_and_switch()                 │
│           → swapcontext(A → B)   // ❷ 切换到 Task B  │
│                                                     │
├─ Task B 运行 (SIGALRM unblocked) ✅ 可被抢占 ────────┤
│   ... B 做了些事，可能被抢占，可能阻塞 ...             │
│   ... 最终 A 被恢复 ...                              │
│                                                     │
│       ← swapcontext 返回        // ❸ A 在此恢复      │
│       → sigprocmask(BLOCK)      // ❹ 重新阻塞 SIGALRM│
│   ← 返回到 tick_signal_handler                      │
├─ 信号处理函数返回                                    │
│   → sigreturn 恢复 A 的原始掩码 (unblocked)          │
└─────────────────────────────────────────────────────┘
  │
  ▼
Task A 继续运行 (SIGALRM unblocked) ✅
```

**四个关键步骤**：

| 步骤 | 操作 | 为什么 |
|------|------|--------|
| ❶ | `sigprocmask(UNBLOCK)` | 信号处理函数中 SIGALRM 被内核自动阻塞。不手动解阻塞的话，swapcontext 过去的 Task B 会带着阻塞的 SIGALRM 运行——它收不到任何 tick 信号，无法再被抢占 |
| ❷ | `swapcontext(A→B)` | 保存 A 的上下文（含信号栈帧），恢复 B 的上下文。A 的 swapcontext 调用"暂停"在这里 |
| ❸ | swapcontext 返回 | 当 A 被重新调度时，执行从这里继续——仿佛 swapcontext 只是一个"耗时很长的函数" |
| ❹ | `sigprocmask(BLOCK)` | 恢复信号处理函数的预期掩码。sigreturn 会基于此恢复 A 的原始掩码 |

**为什么不在 schedule() 中做这些？**

因为 `schedule()` 是从任务上下文调用的（task_delay / task_yield / idle）。此时 SIGALRM 的状态由 `port_disable_interrupts()` / `port_enable_interrupts()` 管理，不需要 `schedule_from_isr()` 的额外掩码操作。两个入口各有各的掩码管理策略。

### 9.5 异步信号安全性分析

从信号处理函数调用 `schedule_from_isr()` 的整个调用链中，每个操作都需要经受异步信号安全的检验：

| 操作 | 是否安全 | 理由 |
|------|----------|------|
| `tick_pending = 1` | ✅ 安全 | `volatile int` 赋值 |
| `tick_count++` | ✅ 安全 | 整数算术 |
| `list_for_each_safe` 遍历 | ✅ 安全 | 纯指针遍历，不分配内存 |
| `list_remove` / `list_insert_tail` | ✅ 安全 | 纯指针操作（改 prev/next），不调 malloc |
| `task->delay_ticks--` | ✅ 安全 | 整数算术 |
| `task->state = ...` | ✅ 安全 | 整数赋值 |
| `find_highest_priority_task()` | ✅ 安全 | 仅链表判空（指针比较）+ `list_first`（指针解引用）+ `list_entry`（指针算术） |
| `sigprocmask` | ✅ 安全 | **POSIX 明确列为异步信号安全** |
| `swapcontext` | ✅ 实践安全 | POSIX 未列入但被 GNU Pth、libaco 等用户态线程库广泛从信号处理器调用 |
| `printf` / `fprintf` / `malloc` | ❌ 不安全 | **已从共享代码路径中移除** |

**关键安全保证**：`schedule_select_and_switch()` 和 `process_tick()` 的代码路径不含任何 `printf`、`malloc`、`fprintf` 调用。唯一的"灰色地带"是 `swapcontext`，它在 macOS/Linux 上从信号处理器调用的可靠性已被十余年的用户态线程库实践验证。

**临界区保护仍然生效**：`port_disable_interrupts()` 在 `task_delay()` 和 `schedule()`（任务上下文入口）中阻塞 SIGALRM。如果代码正在操作就绪链表（如在 `task_delay` 的临界区内），SIGALRM 被阻塞，信号处理函数不会触发。信号只在安全窗口（链表操作完成后、`port_enable_interrupts()` 调用后）才会被递送。

### 9.6 修改的代码文件一览

| 文件 | 改动 |
|------|------|
| `kernel.h` | 新增 `void schedule_from_isr(void);` 声明 |
| `kernel.c` | 新增 `schedule_select_and_switch()` 静态函数；新增 `schedule_from_isr()`；简化 `schedule()`；添加 `#include <signal.h>`（Unix） |
| `port.c` | `tick_signal_handler()` 从"只设标志"改为直接调用 `schedule_from_isr()`；添加 Windows 限制注释；添加 `extern schedule_from_isr` 声明 |
| `demo_preempt_true.c` | **新建**：真抢占验证 demo（CPU_BURN 死循环永不 yield，PREEMPT 通过 ISR 抢占） |
| `Makefile` | 添加 `demo_preempt_true` 构建目标 |

### 9.7 真抢占 Demo 验证

`demo_preempt_true.c` 设计为**只有真抢占才能通过**的测试：

```c
/* CPU_BURN: 纯死循环，从不调用 task_delay 或 task_yield */
static void task_cpu_burn_func(void) {
    volatile uint64_t counter = 0;
    while (1) {
        counter++;  // 永不主动让出 CPU
    }
}

/* PREEMPT: 高优先级，每 10 ticks 醒来 */
static void task_preempt_func(void) {
    while (1) {
        printf("[PREEMPT] tick=%u\n", tick_count);
        task_delay(10);  // 只有这里会阻塞
    }
}
```

**如果是 Phase 6（延迟抢占）**：CPU_BURN 启动后进入死循环，`schedule()` 永远不会被调用。PREEMPT 的 delay 到期后虽然被 `process_tick()` 移回就绪链表，但没有调度点来执行 `schedule()` → `swapcontext()`。**PREEMPT 永远不会打印。**

**如果是 Phase 7（真抢占）**：tick 10 时，SIGALRM 信号处理函数直接调用 `schedule_from_isr()` → `process_tick()` 唤醒 PREEMPT → `schedule_select_and_switch()` 发现 PREEMPT 优先级更高 → `swapcontext(CPU_BURN → PREEMPT)`。**PREEMPT 准时在 tick 10 打印！**

实际运行输出：

```
[CPU_BURN] tick=5     counter=50000001    ← CPU_BURN 在死循环中
[CPU_BURN] tick=8     counter=100000002   ← 仍然在死循环
⚡ [PREEMPT] tick=10   elapsed=10 ticks   ← 抢占成功！CPU_BURN 从未调用 task_delay！
[CPU_BURN] tick=11    counter=150000003   ← PREEMPT delay 后回到 CPU_BURN
[CPU_BURN] tick=14    counter=200000004   ← 继续死循环
⚡ [PREEMPT] tick=20   elapsed=10 ticks   ← 再次准时抢占！
```

关键证据：**`elapsed=10 ticks`** — PREEMPT 每次恰好间隔 10 ticks 出现，而 CPU_BURN 的 counter 在两次 PREEMPT 之间持续递增。这证明：
1. CPU_BURN 确实在不间断地运行（counter 在涨）
2. PREEMPT 被信号处理函数从 CPU_BURN 手中"强行"抢走了 CPU
3. PREEMPT 运行完后，CPU_BURN 从被中断的位置恢复继续（counter 值连续）

### 9.8 延迟抢占 vs 真抢占：一句话总结

```
延迟抢占 (Phase 6):
  "我会在下个路口停车换人，但你要等到我主动开到这个路口。"
  抢占触发点 = task_delay() / task_yield() / idle 轮询

真抢占 (Phase 7):
  "不管你在高速公路上开多快，我直接拉手刹换人。"
  抢占触发点 = 任意时刻（信号/中断随时到达）
```

### 9.9 Windows 平台说明

真抢占（ISR 驱动的上下文切换）目前仅在 Unix 平台上实现。Windows 的 tick 机制使用独立的后台线程（`CreateThread`），`SwitchToFiber()` 只能由拥有该 fiber 的线程调用——后台 tick 线程无法直接切换主线程的 fiber。因此 Windows 上保持 Phase 6 的延迟抢占模式，依赖 idle 任务的 `tick_pending` 轮询。`schedule_from_isr()` 中的 `sigprocmask` 调用已用 `#ifndef _WIN32` 保护。

### 9.10 学习检查点

运行真抢占 demo：
```bash
make clean && make DEMO=demo_preempt_true && ./build/lite_rtos_demo
```

关键观察：
```
⚡ [PREEMPT]  tick=10   elapsed=10 ticks    ← CPU_BURN 从未 yield，但 PREEMPT 出现了！
⚡ [PREEMPT]  tick=20   elapsed=10 ticks    ← 精确的 10 tick 间隔
⚡ [PREEMPT]  tick=30   elapsed=10 ticks    ← 证明 ISR 直接完成了调度
```

如果看不到 PREEMPT 的输出，说明抢占没有发生——检查是否正确修改了 `tick_signal_handler()`。

---

## 10. 完整执行流程追踪

本节从头到尾追踪一次完整的任务执行流程，涵盖延迟抢占和真抢占两种路径。

### 10.1 启动流程

```
main()
  │
  ├─ kernel_init()                    // 初始化数据结构
  │   ├─ 初始化 ready_lists[0..7]     // 8 个空链表
  │   ├─ 初始化 delayed_list          // 空延时链表
  │   ├─ 创建 idle 任务               // 静态 TCB + 静态栈
  │   │   ├─ port_init_task_context() // 设置 idle 的 ucontext
  │   │   └─ task_add_to_ready()      // 挂到 ready_lists[7]
  │   └─ printf("Kernel initialized")
  │
  ├─ task_create(task_a_func, "task_a", 8192, 1)  // 创建用户任务 A
  │   ├─ malloc(TCB)                              // 分配 TCB
  │   ├─ malloc(8192)                             // 分配独立栈
  │   ├─ memset(TCB, 0) + set fields              // 初始化 TCB
  │   ├─ port_init_task_context()                 // 设置 ucontext:
  │   │   ├─ getcontext() → 获取当前寄存器快照
  │   │   ├─ uc_stack.ss_sp = task->stack         // 替换栈指针
  │   │   └─ makecontext(&ctx, task_a_func, 0)    // 设置入口函数
  │   └─ task_add_to_ready()                      // 挂到 ready_lists[1]
  │
  ├─ task_create(task_b_func, "task_b", 8192, 1)  // 创建用户任务 B
  │   └─ ...同上...
  │
  ├─ kernel_start()                    // 启动调度器！
  │   ├─ port_init_tick_timer()        // 开始 100Hz 定时器
  │   ├─ scheduler_running = 1
  │   ├─ first = find_highest_priority_task()
  │   │   扫描 ready_lists[0]→空, [1]→task_a (返回)
  │   ├─ current_task = task_a
  │   ├─ task_a->state = TASK_RUNNING
  │   └─ port_start_first_task(task_a)
  │       └─ setcontext(&task_a->context)
  │           ├─ 切换栈指针 → task_a->stack
  │           ├─ 恢复寄存器
  │           └─ 跳转到 task_a_func()
  │
  └─ // 永远不会执行到这里
```

### 10.2 任务切换流程 (延迟抢占路径)

```
Task A (task_a_func 运行中)
  │
  ├─ printf("Task A iteration N")
  ├─ task_yield()
  │   ├─ port_disable_interrupts()         // 阻塞 SIGALRM
  │   ├─ task_remove_from_ready(A)         // 从 ready_lists[1] 移除 A
  │   ├─ task_add_to_ready(A)              // 加到 ready_lists[1] 尾部
  │   │   ready_lists[1]: sentinel → B → A → sentinel
  │   ├─ port_enable_interrupts()
  │   └─ schedule()
  │       ├─ port_disable_interrupts()
  │       ├─ process_tick() (如果需要)
  │       ├─ next = find_highest_priority_task()
  │       │   扫描: ready_lists[0]→空, [1]→B (返回)
  │       ├─ next (B) != current_task (A)? YES
  │       ├─ prev = A, current_task = B
  │       ├─ A->state = TASK_READY
  │       ├─ B->state = TASK_RUNNING
  │       ├─ port_enable_interrupts()
  │       └─ port_switch_context(A, B)
  │           └─ swapcontext(&A.context, &B.context)
  │               ① 保存所有寄存器 → A.context
  │               ② 恢复 B.context 的寄存器
  │               ③ 跳转到 B 上次停下的位置
  │                   ┌──────────────────────────┐
  │                   │  Task B (task_b_func)    │
  │                   │  printf("Task B it...")  │
  │                   │  ...                     │
  │                   │  task_yield()            │
  │                   │   ...                    │
  │                   │   schedule()             │
  │                   │    ...                   │
  │                   │    next = A              │
  │                   │    swapcontext(B, A)     │
  │                   └──────────────────────────┘
  │              ④ A 从这里继续 (swapcontext 返回)
  │
  ├─ // A 继续执行 task_yield() 之后的代码
  └─ // 回到 while(1) 循环，下一轮迭代
```

### 10.3 Tick 处理流程 (延迟抢占路径)

```
[硬件定时器] 每 10ms 触发 SIGALRM
  │
  ├─ (内核保存当前任务的所有寄存器到栈上)
  ├─ tick_signal_handler(SIGALRM)
  │   └─ tick_pending = 1;   // 只设置标志，不操作数据结构
  ├─ (内核恢复寄存器)
  └─ 返回到被中断的代码继续执行

... 稍后，某个任务调用 task_delay() 或 task_yield() ...
  │
  └─ schedule()
      ├─ port_disable_interrupts()       // 现在安全地屏蔽信号
      ├─ if (port_tick_pending())
      │   └─ process_tick()
      │       ├─ tick_count++            // tick_count: N → N+1
      │       ├─ 遍历 delayed_list
      │       │   ├─ task_H: delay_ticks: 5→4 (未到期)
      │       │   └─ task_L: delay_ticks: 1→0 (到期!)
      │       │       ├─ list_remove(L 的 node)
      │       │       └─ task_add_to_ready(L)
      │       │           L.state = TASK_READY
      │       │           → ready_lists[L.priority]
      │       └─ port_clear_tick()
      ├─ 选择最高优先级任务 (可能是刚唤醒的 L)
      └─ port_switch_context() (如果需要)
```

### 10.4 ISR 真抢占路径 (Phase 7)

```
[硬件定时器] 每 10ms 触发 SIGALRM
  │
  ├─ (内核保存当前任务的所有寄存器到信号栈帧)
  ├─ tick_signal_handler(SIGALRM)
  │   ├─ tick_pending = 1;
  │   └─ schedule_from_isr()
  │       ├─ process_tick()            // 遍历延时链表，唤醒到期任务
  │       │   ├─ tick_count++
  │       │   ├─ task_H: delay_ticks: 1→0 (到期!)
  │       │   │   ├─ list_remove(H 的 node)
  │       │   │   └─ task_add_to_ready(H)  // H 回到 ready_lists[0]
  │       │   └─ task_L: delay_ticks: 2→1 (未到期)
  │       │
  │       ├─ sigprocmask(SIG_UNBLOCK)  // 解阻塞 SIGALRM
  │       │                            // 确保切换到的任务可被抢占
  │       │
  │       ├─ schedule_select_and_switch()
  │       │   ├─ next = find_highest_priority_task()
  │       │   │   扫描: ready_lists[0]→H! 优先级 0 > 当前(LOW=3)
  │       │   │
  │       │   ├─ prev = current_task (LOW)
  │       │   ├─ prev->state = TASK_READY
  │       │   ├─ next->state = TASK_RUNNING
  │       │   ├─ current_task = H
  │       │   │
  │       │   └─ port_switch_context(LOW, H)
  │       │       └─ swapcontext(&LOW.context, &H.context)
  │       │          ① 保存当前 CPU 寄存器 → LOW.context
  │       │             (保存的是信号处理函数内的执行状态!)
  │       │          ② 恢复 H.context 的寄存器
  │       │             (H 从上次 task_delay 的 swapcontext 后继续)
  │       │          ③ 跳转到 H 的代码
  │       │
  │       │   ┌──────────────────────────────────────────┐
  │       │   │  Task H (task_high_func)                 │
  │       │   │  printf("[HIGH] tick=%u\n", tick_count)  │
  │       │   │  task_delay(10)                          │
  │       │   │    → schedule()                          │
  │       │   │      → port_disable_interrupts()         │
  │       │   │      → process_tick() (如果需要)          │
  │       │   │      → port_enable_interrupts()          │
  │       │   │      → schedule_select_and_switch()      │
  │       │   │        → next = LOW (idle 不参与)        │
  │       │   │        → swapcontext(H, LOW)             │
  │       │   └──────────────────────────────────────────┘
  │       │
  │       │  ④ LOW 从 swapcontext 返回 (在 schedule_from_isr 中!)
  │       │     ↓
  │       ├─ sigprocmask(SIG_BLOCK)    // 恢复信号处理函数掩码
  │       │
  │       └─ 返回到 tick_signal_handler
  │
  ├─ tick_signal_handler 返回
  ├─ sigreturn: 内核恢复 LOW 的原始寄存器状态 (SIGALRM unblocked)
  └─ LOW 从被 SIGALRM 打断的位置继续执行
       (仿佛只是"愣了一瞬间"，完全不知道 H 已经运行过)
```

**关键观察**：

1. LOW 被 SIGALRM 打断时正在执行任意代码（死循环 `counter++`），没有调用任何内核 API
2. 信号处理函数直接完成了 tick 处理 + 调度决策 + 上下文切换
3. `swapcontext` 保存的不是"LOW 的任务代码执行位置"，而是"信号处理函数内的执行位置"——这包含了信号栈帧，恢复时 sigreturn 自然把 LOW 放回被中断的指令之后
4. LOW "感觉"自己只是被暂停了一瞬间，完全不知道 H 已经运行过

这就是真正的抢占——**不需要任务配合，内核在中断上下文中强制切换。**

---

## 11. 与 FreeRTOS 的对比

| 概念 | Lite RTOS | FreeRTOS |
|------|-----------|----------|
| 任务控制块 | `tcb_t` | `TCB_t` (tasks.c) |
| 就绪链表 | `ready_lists[N]` | `pxReadyTasksLists[N]` |
| 延时链表 | `delayed_list` | `xDelayedTaskList1/2` |
| 链表节点 | `list_node_t` | `xLIST_ITEM` |
| 链表头 | `list_t` (带 sentinel) | `xLIST` (带 xListEnd) |
| 反查宏 | `list_entry()` | `listGET_OWNER_OF_NEXT_ENTRY()` |
| 调度器 (任务上下文) | `schedule()` | `vTaskSwitchContext()` |
| 调度器 (ISR 上下文) | `schedule_from_isr()` | `vTaskSwitchContext()` (PendSV 中) |
| 真抢占 | ✅ ISR 直接调用 `schedule_from_isr()` | ✅ SysTick → PendSV |
| 任务延时 | `task_delay()` | `vTaskDelay()` |
| 空闲任务 | `idle_task` (自动创建) | `prvIdleTask` (自动创建) |
| 上下文切换 | `swapcontext()` | PendSV 汇编 |
| 临界区 | `port_disable_interrupts()` | `taskENTER_CRITICAL()` |
| 系统节拍 | SIGALRM (100Hz) | SysTick (通常 1000Hz) |
| 优先级位图 | ❌ (遍历 8 个优先级) | ✅ `uxTopReadyPriority` (O(1)) |
| 时间片轮转 | ❌ | ✅ (同优先级按时间片) |
| 互斥量 | ❌ | ✅ |
| 信号量 | ❌ | ✅ |
| 消息队列 | ❌ | ✅ |
| 内存管理 | malloc/free | heap_1~heap_5 |

---

## 12. 扩展方向

### 12.1 优先级位图加速

当前 `find_highest_priority_task()` 每次扫描 8 个优先级。用位图只需一条 `__builtin_clz()` 指令：

```c
static uint32_t ready_bitmap = 0;

void task_add_to_ready(tcb_t *task) {
    task->state = TASK_READY;
    list_insert_tail(&ready_lists[task->priority], &task->node);
    ready_bitmap |= (1 << task->priority);  // 设置对应位
}

static tcb_t *find_highest_priority_task(void) {
    // __builtin_clz: 前导零计数
    // 位图中最高位 = 最高优先级就绪任务
    int prio = 31 - __builtin_clz(ready_bitmap);
    return list_entry(list_first(&ready_lists[prio]), tcb_t, node);
}
```

### 12.2 时间片轮转

同优先级任务各执行 1 个 tick，然后自动切换到同优先级的下一个：

```c
// 在 process_tick() 中：
if (当前任务优先级有其他就绪任务 && 时间片用完) {
    task_remove_from_ready(current_task);
    task_add_to_ready(current_task);  // 移到链表尾部
    schedule();  // 触发同优先级切换
}
```

### 12.3 信号量 (Semaphore)

```c
typedef struct {
    int count;              // 信号量计数
    list_t wait_list;       // 等待此信号量的任务链表
} semaphore_t;

void sem_take(semaphore_t *sem) {
    port_disable_interrupts();
    if (sem->count > 0) {
        sem->count--;
    } else {
        // 资源不可用：阻塞当前任务，加入等待链表
        task_remove_from_ready(current_task);
        current_task->state = TASK_BLOCKED;
        list_insert_tail(&sem->wait_list, &current_task->node);
        port_enable_interrupts();
        schedule();  // 切换到其他任务
    }
    port_enable_interrupts();
}

void sem_give(semaphore_t *sem) {
    port_disable_interrupts();
    if (!list_is_empty(&sem->wait_list)) {
        // 有任务在等待：唤醒等待队列中优先级最高的
        list_node_t *n = list_pop_head(&sem->wait_list);
        tcb_t *task = list_entry(n, tcb_t, node);
        task_add_to_ready(task);  // 唤醒
    } else {
        sem->count++;
    }
    port_enable_interrupts();
}
```

### 12.4 ARM Cortex-M3 移植

替换 `port.c` 中的 ucontext 操作为：

```assembly
/* PendSV 异常处理函数 */
PendSV_Handler:
    /* 保存上文 */
    mrs     r0, psp                 /* 获取任务栈指针 */
    stmdb   r0!, {r4-r11}          /* 保存 R4-R11 到任务栈 */
    str     r0, [r1]               /* 保存栈指针到 TCB */

    /* 加载下文 */
    ldr     r0, =current_task
    ldr     r2, [r0]               /* 新任务的 TCB 指针 */
    ldr     r0, [r2]               /* 新任务的栈指针 */
    ldmia   r0!, {r4-r11}          /* 恢复 R4-R11 */
    msr     psp, r0                /* 更新 PSP */
    bx      lr                     /* 返回，硬件自动恢复 R0-R3,R12,LR,PC,xPSR */
```

### 12.5 调试增强

```c
// 打印所有任务状态 (调试神器)
void task_dump_all(void) {
    printf("\n=== Task Dump (tick=%u) ===\n", tick_count);
    printf("%-16s %-8s %-8s %s\n", "Name", "Prio", "State", "Delay");
    printf("──────────────────────────────────\n");

    // 遍历所有优先级
    for (int p = 0; p < MAX_PRIORITIES; p++) {
        list_node_t *n;
        list_for_each(n, &ready_lists[p]) {
            tcb_t *t = list_entry(n, tcb_t, node);
            printf("%-16s %-8u READY\n", t->name, t->priority);
        }
    }

    // 遍历延时链表
    {
        list_node_t *n;
        list_for_each(n, &delayed_list) {
            tcb_t *t = list_entry(n, tcb_t, node);
            printf("%-16s %-8u BLOCKED  %u\n", t->name, t->priority, t->delay_ticks);
        }
    }

    if (current_task) {
        printf("%-16s %-8u RUNNING ← current\n",
               current_task->name, current_task->priority);
    }
    printf("==================================\n\n");
}
```

---

## 附录 A：编译与运行

### macOS / Linux

```bash
# 编译 (零警告)
make clean && make

# 运行
./build/lite_rtos_demo

# 编译标志说明:
#   -Wall -Wextra -Wshadow    严格警告
#   -Wno-deprecated-declarations  ucontext 在 macOS 上已弃用
#   -D_XOPEN_SOURCE=600        启用 ucontext POSIX API
```

### Windows (MSYS2)

```bash
# 在 MSYS2 MinGW-w64 终端中:
cd /c/Users/xxx/lite_rtos
make clean && make

# 运行
./build/lite_rtos_demo.exe

# MSYS2 安装:
#   1. 下载 https://www.msys2.org/
#   2. 安装后打开 "MSYS2 MinGW 64-bit" 终端
#   3. pacman -S mingw-w64-x86_64-gcc make
```

## 附录 B：关键文件阅读顺序

**如果你想从零理解这个 RTOS 内核，按以下顺序读代码：**

| 顺序 | 文件 | 关注点 |
|------|------|--------|
| 1 | `kernel.h` | 类型定义、TCB 结构体、任务状态、list_entry 宏 |
| 2 | `list.h` | 链表 API 声明 |
| 3 | `list.c` | 链表实现——理解侵入式链表的 4 个操作 |
| 4 | `task.h` | 任务管理 API |
| 5 | `task.c` | `task_create()` 的 4 个步骤、`task_delay()` 的 5 个步骤 |
| 6 | `port.h` | 平台移植 API |
| 7 | `port.c` | `port_init_task_context()` → `port_start_first_task()` → `port_switch_context()` |
| 8 | `kernel.c` | `kernel_init()` → `kernel_start()` → `schedule()` → `process_tick()` → `schedule_from_isr()` |
| 9 | `demo_preempt.c` | 看延迟抢占如何工作（Phase 6） |
| 10 | `demo_preempt_true.c` | 看真抢占如何工作（Phase 7）— CPU 密集型任务被 ISR 强制切换 |
| 11 | 全部重读一遍 | 第二次阅读会有全新的理解 |

## 附录 C：常见面试/理解问题

1. **为什么每个任务需要独立栈？** → 见 §5.4
2. **侵入式链表相比普通链表的优势？** → 见 §3.2
3. **为什么要用哨兵节点？** → 见 §3.2
4. **schedule() 为什么不会返回？不对——它什么时候返回？** → schedule() 在不需要切换时返回；需要切换时通过 swapcontext() 在另一个任务中"返回"
5. **tick handler 为什么只设置标志，不直接处理？** → 见 §7.5
6. **PC 上的抢占和 ARM 硬件上的抢占有什么区别？** → 见 §8.3
7. **task_delay(0) 会发生什么？** → 直接返回，不阻塞
8. **如果所有任务都阻塞了会怎样？** → 空闲任务 (idle) 始终就绪，CPU 不会停止
9. **swapcontext 保存了多少寄存器？** → 所有通用寄存器 (RIP, RSP, RBP, RAX, RBX, RCX, RDX, RSI, RDI, R8-R15, 标志寄存器等)
10. **如何验证抢占确实发生了？** → Phase 6 demo 中 HIGH 任务每 10 tick 精确出现在输出中，即使 LOW 每 3 tick 运行一次
11. **延迟抢占和真抢占有什么区别？** → 延迟抢占只在 task_delay/yield/idle 轮询时发生，真抢占在 ISR 中直接完成上下文切换。详见 §9.8
12. **为什么真抢占需要 sigprocmask 操作？** → 信号处理函数中 SIGALRM 被内核自动阻塞，不手动解阻塞的话切换到的任务无法再被抢占。详见 §9.4
13. **Windows 上为什么没有真抢占？** → `SwitchToFiber()` 只能由拥有该 fiber 的线程调用，后台 tick 线程无法操作主线程的 fiber。详见 §9.9
14. **schedule() 和 schedule_from_isr() 的区别？** → schedule() 在任务上下文中调用，自己负责开关中断；schedule_from_isr() 在 ISR 上下文中调用，额外管理信号掩码。详见 §9.3
