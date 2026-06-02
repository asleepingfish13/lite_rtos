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
8. [Phase 6：抢占式调度](#8-phase-6抢占式调度)
9. [完整执行流程追踪](#9-完整执行流程追踪)
10. [与 FreeRTOS 的对比](#10-与-freertos-的对比)
11. [扩展方向](#11-扩展方向)

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

### 5.2 ucontext 机制详解

在 ARM 硬件上，上下文切换用汇编操作 PendSV 异常实现。在 PC 上，我们用 POSIX 的 `ucontext.h` 模拟。

**`getcontext`**：获取当前 CPU 寄存器状态，保存到 `ucontext_t` 中。

**`makecontext`**：修改 `ucontext_t`，设置入口函数和栈指针。调用后，当这个 context 被激活时，CPU 会从指定的函数开始执行。

**`swapcontext(from, to)`**：等价于 `getcontext(from)` + `setcontext(to)`。保存当前状态到 `from`，恢复 `to` 的状态。

**`setcontext(ctx)`**：跳转到 `ctx` 保存的状态，不保存当前状态（"一去不回"）。

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
    port_clear_tick();
}
```

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

### 7.5 信号安全：为什么 tick handler 只设置标志？

```c
static void tick_signal_handler(int sig) {
    tick_pending = 1;   // 仅此而已！
}
```

`SIGALRM` 可能在**任何时刻**触发——包括正在 `list_insert_tail()` 的中间。如果 handler 中直接操作链表，会导致数据损坏。因此 handler 只设置 `sig_atomic_t` 标志，实际的链表操作推迟到 `schedule()`（中断被阻塞时）进行。

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

## 8. Phase 6：抢占式调度

### 8.1 合作式 vs 抢占式

```
合作式调度 (Phase 1-5):
  任务必须主动 task_yield() 或 task_delay()，其他任务才能运行。
  如果某个任务写了 while(1) {} 而不 yield，整个系统卡死。

抢占式调度 (Phase 6):
  高优先级任务就绪时，内核可以"强制"暂停当前任务。
  在 PC 模拟中，抢占发生在内核 API 调用边界 (task_delay/yield)。
  在真实硬件上，中断返回时通过 PendSV 实现即时抢占。
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
| 抢占时机 | 下一个内核 API 调用 | SysTick ISR 返回时立即触发 PendSV |
| 临界区 | 阻塞 SIGALRM | 禁用中断 (BASEPRI / PRIMASK) |

**为什么 PC 上不是在任意时刻抢占？** 因为信号 handler 中操作内核数据结构不安全（不是异步信号安全的）。真实硬件没有这个问题——中断 handler 中可以直接操作就绪链表，因为硬件保证中断上下文不会被其他中断打断（同优先级或更低优先级的中断被屏蔽）。

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

## 9. 完整执行流程追踪

本节从头到尾追踪一次完整的任务执行流程。

### 9.1 启动流程

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

### 9.2 任务切换流程

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

### 9.3 Tick 处理流程

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

---

## 10. 与 FreeRTOS 的对比

| 概念 | Lite RTOS | FreeRTOS |
|------|-----------|----------|
| 任务控制块 | `tcb_t` | `TCB_t` (tasks.c) |
| 就绪链表 | `ready_lists[N]` | `pxReadyTasksLists[N]` |
| 延时链表 | `delayed_list` | `xDelayedTaskList1/2` |
| 链表节点 | `list_node_t` | `xLIST_ITEM` |
| 链表头 | `list_t` (带 sentinel) | `xLIST` (带 xListEnd) |
| 反查宏 | `list_entry()` | `listGET_OWNER_OF_NEXT_ENTRY()` |
| 调度器 | `schedule()` | `vTaskSwitchContext()` |
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

## 11. 扩展方向

### 11.1 优先级位图加速

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

### 11.2 时间片轮转

同优先级任务各执行 1 个 tick，然后自动切换到同优先级的下一个：

```c
// 在 process_tick() 中：
if (当前任务优先级有其他就绪任务 && 时间片用完) {
    task_remove_from_ready(current_task);
    task_add_to_ready(current_task);  // 移到链表尾部
    schedule();  // 触发同优先级切换
}
```

### 11.3 信号量 (Semaphore)

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

### 11.4 ARM Cortex-M3 移植

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

### 11.5 调试增强

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
| 8 | `kernel.c` | `kernel_init()` → `kernel_start()` → `schedule()` → `process_tick()` |
| 9 | `demo.c` | 看一个完整的应用如何使用这些 API |
| 10 | 全部重读一遍 | 第二次阅读会有全新的理解 |

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
