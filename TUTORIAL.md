# Lite RTOS 从零学习教程

> 一个模仿 FreeRTOS 实现的微型实时操作系统内核，总计约 1500 行 C 代码。
> 本文档带你从零开始理解每一行代码的设计意图。

---

## 目录

0. [新手入门：内核运行完整流程](#0-新手入门内核运行的完整流程10-分钟预览)
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
11. [Phase 9：可变 Tick 间隔 + 毫秒级延时](#11-phase-9可变-tick-间隔--毫秒级延时)
12. [Phase 10：任务数量上限控制](#12-phase-10任务数量上限控制)
13. [Phase 11：计数信号量 (Counting Semaphore)](#13-phase-11计数信号量-counting-semaphore)
14. [与 FreeRTOS 的对比](#14-与-freertos-的对比)
15. [扩展方向](#15-扩展方向)
16. [附录 A：编译与运行](#附录-a编译与运行)
17. [附录 B：关键文件阅读顺序](#附录-b关键文件阅读顺序)
18. [附录 C：常见面试/理解问题](#附录-c常见面试理解问题)

---

## 0. 新手入门：内核运行的完整流程（10 分钟预览）

如果你是第一次接触 RTOS，建议先读这一节——不深究细节，只看"内核从头到尾做了什么"。读完你会对后续各 Phase 的内容有一个全局框架。

### 从 `main()` 到第一个任务

```
main()
  │
  ├─ kernel_init()                  ← ① 初始化: 创建就绪链表、延时链表、idle 任务
  │
  ├─ kernel_set_tick_interval_ms(10) ← ② 配置: 设系统心跳为 10ms/次
  │
  ├─ task_create(task_a, "A", 0, 0) ← ③ 创建任务: 分配 TCB + 独立栈 + 初始化 CPU 上下文
  ├─ task_create(task_b, "B", 0, 2) ←    每个任务都会调用 port_init_task_context()
  │                                   设置 "第一次运行的现场"
  │
  ├─ kernel_start()                 ← ④ 启动!
  │   ├─ port_init_tick_timer()     ←    开启 10ms 定时器 (硬件心跳)
  │   ├─ find_highest_priority_task() ←  选优先级最高的就绪任务
  │   └─ port_start_first_task(A)   ←    跳转到 Task A 的代码!
  │
  └─ // 永远不会执行到这里
```

### 任务运行时：上下文切换的全貌

当 Task A 正在运行，它调用 `task_delay(5)`（"我要等 5 个 tick"）：

```
Task A 运行中
  │
  ├─ task_delay(5)
  │   ├─ port_disable_interrupts()     ← 关闭中断, 保护内核数据
  │   ├─ task_remove_from_ready(A)     ← 把 A 从就绪链表摘掉
  │   ├─ A->state = TASK_BLOCKED       ← 标记"我在等"
  │   ├─ A->delay_ticks = 5            ← 设倒计时
  │   ├─ list_insert_tail(delayed_list, &A->node)  ← A 挂到延时链表
  │   ├─ port_enable_interrupts()      ← 开中断
  │   └─ schedule()                    ← "谁该跑?"
  │       ├─ 扫描 ready_lists[0..7]    ← 从高到低找就绪任务
  │       ├─ 找到 Task B (prio 2)
  │       └─ swapcontext(A, B)         ← ★ CPU 上下文切换!
  │           ├─ 保存 A 的所有寄存器 → A.context
  │           └─ 恢复 B 的所有寄存器 ← B.context
  │
Task B 开始运行 (从上次停下的地方继续)
  │
  ├─ ... B 做了些事 ...
  ├─ task_delay(3)                     ← B 也延时
  │   └─ schedule() → 只剩 idle → swapcontext(B, idle)
  │
idle 任务运行                           ← CPU 永不停歇
  │   └─ 循环: 检查 tick_pending, 有就处理
  │
  ├─ [10ms 到!]  SIGALRM 信号触发
  │   └─ schedule_from_isr()
  │       ├─ process_tick()            ← tick_count++; 遍历延时链表
  │       │   ├─ A->delay_ticks: 5→4  ← 还没到
  │       │   └─ B->delay_ticks: 3→2  ← 还没到
  │       ├─ find_highest_priority_task() → idle (仍在就绪)
  │       └─ swapcontext(idle, idle)   ← 无切换, 继续 idle
  │
  ├─ [又 10ms]  SIGALRM → process_tick() → A:4→3, B:2→1
  ├─ [又 10ms]  SIGALRM → process_tick() → A:3→2, B:1→0 ★
  │                                       → B 到期! 放回 ready_lists[2]
  │                                       → swapcontext(idle, B) ← 切到 B!
Task B 继续运行 (从 task_delay 之后)
```

### 多任务数据流：信号量如何工作

```
Task A "IR 传感器" (prio 0)          Task B "障碍分析" (prio 2)
  │                                      │
  ├─ 采集 ir_distance=150                ├─ sem_take(&sem_data) → count=0
  ├─ sem_give(&sem_data)                 │   加入 sem_data.wait_list (排队)
  │   └─ 弹出 B, task_add_to_ready(B)    │   schedule() → 切走
  │                                      │   (B 在此冻结)
  ├─ task_delay_ms(50)                   │
  │   └─ schedule() → 选 B (prio 2)      │
  │                                      ├─ ★ B 被唤醒!
  │                                      ├─ 读取 ir_distance=150
  │                                      ├─ 分析: 安全 (≥100)
  │                                      └─ task_delay_ms(20)
```

### 内核的四个核心数据结构

```
ready_lists[0..7]          delayed_list              sem->wait_list
┌─┬─┬─┬─┬─┬─┬─┬─┐         ┌──────────────┐         ┌──────────────┐
│0│1│2│3│4│5│6│7│         │ sentinel     │         │ sentinel     │
│ │ │B│ │ │ │ │i│         │  ↑       ↓   │         │  ↑       ↓   │
│ │ │ │ │ │ │ │d│         │ [A] ←→ [C]   │         │ [B] ←→ [D]   │
│ │ │ │ │ │ │ │l│         └──────────────┘         └──────────────┘
│ │ │ │ │ │ │ │e│          任务在"等时间"             任务在"等事件"
└─┴─┴─┴─┴─┴─┴─┴─┘
  任务在"等 CPU"

一个任务 (TCB) 只能处于其中一个链表中 (TCB 只有一个 node 成员)。
调度器的工作 = 在这三组链表之间搬运任务。
```

### 阅读路线建议

| 你想了解 | 读这个 Phase | 预计时间 |
|----------|-------------|----------|
| 链表怎么串起任务 | Phase 1 (§3) | 15 分钟 |
| 任务如何创建 | Phase 2 (§4) | 10 分钟 |
| CPU 怎么在任务间切换 | Phase 3 (§5) | 20 分钟 |
| 调度器怎么选任务 | Phase 4 (§6) | 10 分钟 |
| 延时和 tick | Phase 5 (§7) | 15 分钟 |
| 抢占 | Phase 6-7 (§8-9) | 20 分钟 |
| 完整执行追踪 | §10 | 15 分钟 |
| 任务数量限制 | Phase 10 (§12) | 10 分钟 |
| 信号量 (任务同步) | Phase 11 (§13) | 25 分钟 |

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
| `demo_*.c` | 示例应用 (多个 Phase 对应不同 Demo) | 你的 `main.c` |

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
    /* --- 上下文信息 (平台相关) --- */
#ifdef _WIN32
    void               *fiber;         // Windows: Fiber 句柄
#else
    ucontext_t          context;        // Unix: CPU 寄存器快照 (RIP, RSP, RBP...)
#endif
    uint8_t            *stack;          // 任务专属栈的起始地址
    uint32_t            stack_size;     // 栈大小 (字节)

    /* --- 任务属性 --- */
    uint8_t             priority;       // 当前优先级 (0 最高)
    uint8_t             base_priority;  // 基础优先级 (预留给互斥量)
    uint8_t             state;          // READY / RUNNING / BLOCKED
    char                name[16];       // 调试用任务名

    /* --- 延时信息 --- */
    uint32_t            delay_ticks;    // 还需等待的 tick 数

    /* --- 链表节点 (嵌入!) --- */
    list_node_t         node;           // 挂在 ready_lists[] 或 delayed_list
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
    if (getcontext(&task->context) == -1) {        // 获取当前 CPU 状态作为模板
        perror("getcontext"); exit(1);
    }
    task->context.uc_stack.ss_sp    = task->stack; // 替换栈指针：指向任务专属栈
    task->context.uc_stack.ss_size  = task->stack_size;
    task->context.uc_stack.ss_flags = 0;           // 无特殊标志
    task->context.uc_link = NULL;                  // 函数返回时线程退出 (不 fall through)
    makecontext(&task->context, (void (*)(void))task_func, 0);  // 设置入口函数
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

这是内核的心脏。当前实现将调度分成两层：

```c
/* 入口层: schedule() — 任务上下文调用 (task_delay / task_yield / idle) */
void schedule(void)
{
    port_disable_interrupts();       // 临界区：保护链表操作

#ifdef _WIN32
    /* Windows: tick 由后台线程设置标志，这里处理 */
    if (port_tick_pending()) { process_tick(); }
#else
    /* Unix: tick 已由 schedule_from_isr() 在信号上下文中处理,
     *       这里仅清除可能残留的标志 (防止关中断期间遗漏的 tick 被重复处理) */
    if (port_tick_pending()) { port_clear_tick(); }
#endif

    port_enable_interrupts();        // 开中断 → 选任务 → 切换
    schedule_select_and_switch();    // ← 委托给共享调度核心
}

/* 核心层: schedule_select_and_switch() — 共享调度核心 (异步信号安全) */
static void schedule_select_and_switch(void)
{
    tcb_t *next = find_highest_priority_task();

    /* idle 任务始终就绪，next 不可能为 NULL */
    if (next == NULL || next == current_task) {
        return;                      // 无需切换
    }

    tcb_t *prev = current_task;

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;    // 前任务变为就绪态
    }
    next->state = TASK_RUNNING;     // 新任务变为运行态
    current_task = next;

    port_switch_context(prev, next); // ← 上下文切换发生在这里！
    // ─────────────────────────────────────
    // prev 下次被调度时从这里继续
    // ─────────────────────────────────────
}
```

### 6.3 task_yield() — 主动让出 CPU

```c
void task_yield(void) {
    port_disable_interrupts();             // ← 临界区：保护就绪链表

    task_remove_from_ready(current_task);  // 从就绪链表移除
    task_add_to_ready(current_task);       // 加到就绪链表尾部

    port_enable_interrupts();              // ← 临界区结束
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
    printf("[kernel] Starting scheduler...\n");

    port_init_tick_timer();              // ← 启动系统节拍定时器!

    scheduler_running = 1;

    tcb_t *first = find_highest_priority_task();  // 选最高优先级
    if (first == NULL) {                          // 安全检查
        fprintf(stderr, "[kernel] ERROR: No task to start!\n");
        exit(1);
    }
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

### 7.2 Tick 间隔的设置

Lite RTOS 提供两种 API 设置系统节拍间隔：

```c
/* 推荐: 毫秒级 API — 直设硬件定时器间隔 */
kernel_set_tick_interval_ms(10);   // 每 10ms 中断一次 (100Hz)
kernel_set_tick_interval_ms(1);    // 每  1ms 中断一次 (1000Hz)

/* 内部 API — 频率方式 (等价) */
kernel_set_tick_rate(100);         // 100Hz = 10ms 间隔
```

硬件层面，这个值直接转换为 SysTick（ARM）或 `setitimer`（Unix）的定时器重装值。例如 480MHz CPU 上设 10ms：

```
SysTick->LOAD = 480000000 × 0.01 - 1 = 4,799,999
```

每个 CPU 时钟周期 LOAD 减 1，减到 0 → 触发中断 → 硬件自动重装 → 软件零开销。

- **太快** (1ms)：每秒 1000 次中断，CPU 开销可忽略（<0.1%），但省电模式下不理想
- **太慢** (1000ms)：每 1 秒一次中断，省电但任务延时精度差
- **默认 10ms**：适合大多数嵌入式应用和教学 demo

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
    if (ticks == 0) return;              // 延时 0 = 无操作

    port_disable_interrupts();           // 临界区

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

### 7.4b task_delay_ms() — 毫秒级延时 (推荐)

`task_delay()` 的参数是 tick 数，具体等多久取决于 tick 间隔——`task_delay(5)` 在 10ms/tick 时 = 50ms，在 1ms/tick 时 = 5ms。为此提供毫秒 API：

```c
// task.c + task.h
void task_delay_ms(uint32_t ms)
{
    if (ms == 0) return;

    /* 毫秒 → tick 换算: ticks = ceil(ms / interval_ms)
     * 内部通过 kernel_get_tick_rate() 获取当前频率:
     *   ticks = (ms * hz + 999) / 1000   (向上取整)
     *
     * 例如 tick 间隔=10ms (hz=100):
     *   ms=50  → ticks=(50*100+999)/1000 = 5   → 实际 50ms
     *   ms=15  → ticks=(15*100+999)/1000 = 2   → 实际 20ms (取整)
     *
     * 例如 tick 间隔=500ms (hz=2):
     *   ms=500 → ticks=(500*2+999)/1000 = 2    → 实际 1000ms
     */
    uint32_t hz = kernel_get_tick_rate();
    uint32_t ticks = ((uint64_t)ms * hz + 999) / 1000;
    if (ticks == 0) ticks = 1;  /* 最小延时 1 tick */

    task_delay(ticks);
}
```

**性能开销**：一次乘法 + 一次除法，在 480MHz Cortex-M7 上 ≈ 3 个 CPU 周期（~6ns），完全可忽略。真正的开销是上下文切换本身（微秒级）。

**设计原则**：`task_delay_ms()` 与 tick 间隔解耦——不管硬件 tick 是快是慢，传 50 就是等 50ms。这让 demo 代码在不同 tick 配置下都能正确运行。

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
static sigset_t block_mask;
static int interrupts_disabled = 0;         // 嵌套计数保护

void port_disable_interrupts(void) {
    if (interrupts_disabled) return;         // 已关中断, 嵌套调用安全
    interrupts_disabled = 1;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGALRM);
    sigprocmask(SIG_BLOCK, &block_mask, NULL);  // 阻塞 SIGALRM
}

void port_enable_interrupts(void) {
    if (!interrupts_disabled) return;        // 已开中断, 嵌套调用安全
    interrupts_disabled = 0;
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

**`schedule()`** — 任务上下文入口（Phase 7 简化后）：

```c
void schedule(void)
{
    port_disable_interrupts();

#ifdef _WIN32
    /* Windows: tick 由后台线程设置标志，这里处理 */
    if (port_tick_pending()) { process_tick(); }
#else
    /* Unix: tick 已由 schedule_from_isr() 在信号上下文中处理，
     *       这里仅清除残留标志，不做重复的 process_tick() */
    if (port_tick_pending()) { port_clear_tick(); }
#endif

    /* 先开中断再切换，确保新任务以中断开启状态运行 */
    port_enable_interrupts();
    schedule_select_and_switch();
}
```

**重要**：在 Unix 真抢占模式下，`schedule()` **不再**调用 `process_tick()`——tick 处理全部在 `schedule_from_isr()`（信号上下文）中完成。`schedule()` 只负责清除可能残余的 `tick_pending` 标志（关中断期间漏掉的 tick）。Windows 上仍然由 `schedule()` 调用 `process_tick()`（没有 ISR 驱动）。

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

### 10.3 Tick 处理流程 (Windows 延迟抢占路径)

Windows 上 tick 由后台线程设置标志，调度依赖任务主动调用 API。
Unix 上 tick 由 SIGALRM 信号驱动，`schedule_from_isr()` 在 ISR 中直接完成（见 §10.4）。

```
[后台线程] 每 10ms Sleep → tick_pending = 1
  │
  └─ ... 稍后，某个任务调用 task_delay() / task_yield() / idle 轮询 ...
       │
       └─ schedule()
           ├─ port_disable_interrupts()
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
           ├─ port_enable_interrupts()
           └─ schedule_select_and_switch()   // 选任务 + 切换
```

> **Unix 真抢占路径**见 §10.4 — tick 在 ISR 中直接处理，`schedule()` 只清除残留标志。

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

## 11. Phase 9：可变 Tick 间隔 + 毫秒级延时

### 推荐先读这些文件

> `port.c` (可变 tick API) → `task.c` (task_delay_ms) → `demo_tick_rate.c`

### 11.1 为什么需要可变 Tick 间隔？

不同应用场景对系统节拍有截然不同的需求：

| 场景 | Tick 间隔 | 频率 | 原因 |
|------|----------|------|------|
| 环境监测 | 500ms～1s | 1～2Hz | 温度/湿度变化慢，低功耗优先 |
| 小车巡线 | 10ms | 100Hz | 实时控制，需快速响应传感器 |
| 通用嵌入式 | 10ms | 100Hz | 大多数场景的平衡点 |
| 高精度控制 | 1ms | 1000Hz | 伺服电机、PID 控制等 |

**核心理念**：硬件只有一个 SysTick（或 `setitimer`），通过 `kernel_set_tick_interval_ms()` 直接把想要的间隔写入定时器寄存器。定时器到时间就触发中断，CPU 处理 tick → 调度 → 回到任务。整个过程中硬件自动运行，不消耗 CPU 指令。

### 11.2 API 设计

```c
/* kernel.h — 推荐: 毫秒级 API */
void     kernel_set_tick_interval_ms(uint32_t ms);  // 1~1000ms
uint32_t kernel_get_tick_interval_ms(void);         // 当前间隔 (ms)

/* task.h — 毫秒级延时 */
void task_delay_ms(uint32_t ms);  // 与 tick 间隔解耦, 始终是真实毫秒
```

**设计原理图和调用链**：

```
用户任务                          内核                         硬件
────────                          ────                         ────
task_delay_ms(100)               
  │                               
  ├─ ticks = (100ms * hz)/1000    
  │   (一次乘除, ~3 CPU cycles)    
  │                               
  └─→ task_delay(ticks)           
        │                         
        ├─ 当前任务 → BLOCKED      
        ├─ 加入 delayed_list      
        └─ schedule() → 切换任务   
                                    │
                                    │  ┌─────────────────────────┐
                                    │  │ SysTick 计数器          │
                                    │  │ 每个 CPU 时钟周期减 1   │
                                    │  │ 减到 0 → 触发中断       │
                                    │  │ 硬件自动重装 LOAD 值    │
                                    │  └─────────────────────────┘
                                    │
                              process_tick()
                                ├─ tick_count++
                                ├─ 遍历 delayed_list
                                ├─ delay_ticks--
                                └─ 到期 → 唤醒任务
```

### 11.3 `kernel_set_tick_interval_ms()` 的实现

```c
/* port.c — 跨平台统一 API, 内部委托给 kernel_set_tick_rate() */
void kernel_set_tick_interval_ms(uint32_t ms)
{
    if (ms < 1 || ms > 1000) return;
    kernel_set_tick_rate(1000 / ms);  // 毫秒 → 频率换算
}

/* 底层实现 (Unix 版本) */
void kernel_set_tick_rate(uint32_t hz)
{
    if (hz < 1 || hz > 1000) return;

    port_disable_interrupts();     // 临界区保护
    g_tick_rate_hz = hz;           // 更新全局变量

    if (scheduler_running) {
        struct itimerval timer;
        uint32_t interval_us = 1000000 / hz;   // 频率 → 微秒

        timer.it_value.tv_sec     = interval_us / 1000000;
        timer.it_value.tv_usec    = interval_us % 1000000;
        timer.it_interval.tv_sec  = interval_us / 1000000;
        timer.it_interval.tv_usec = interval_us % 1000000;

        setitimer(ITIMER_REAL, &timer, NULL);  // 直写硬件定时器
        port_clear_tick();                      // 清除旧频率残留
    }

    port_enable_interrupts();
}
```

**关键设计点**：

1. **`port_disable_interrupts()`**：修改全局变量 `g_tick_rate_hz` 和硬件定时器时需要临界区保护，防止 SIGALRM 在中间打断
2. **`scheduler_running` 检查**：调度器启动前只设全局变量，`kernel_start()` 调用 `port_init_tick_timer()` 时读取并生效
3. **`port_clear_tick()`**：切换间隔后清除旧频率下可能残留的 tick_pending 标志，防止一次旧 tick 触发多余调度
4. **Windows 无额外操作**：tick 线程每循环重新读取 `g_tick_rate_hz`，自动适配新间隔

### 11.4 `task_delay_ms()` — 与 Tick 间隔解耦

这是整个设计的核心价值。对比两种写法：

```c
/* 旧写法: 依赖 tick 间隔, 换间隔后行为变化 */
kernel_set_tick_interval_ms(10);
task_delay(5);    // 5 ticks × 10ms = 50ms  ✓

kernel_set_tick_interval_ms(500);
task_delay(5);    // 5 ticks × 500ms = 2500ms  ← 完全变了!

/* 新写法: 与 tick 间隔无关, 始终是真实毫秒 */
kernel_set_tick_interval_ms(10);
task_delay_ms(50);   // 50ms ✓ (内核换算为 5 ticks)

kernel_set_tick_interval_ms(500);
task_delay_ms(50);   // 50ms ✓ (内核换算为 1 tick, 向上取整)
```

**换算精度**：ms→tick 转换有取整。例如 tick 间隔=10ms 时，`task_delay_ms(15)` = ceil(15/10) = 2 ticks = 20ms。这是固定 tick 间隔的固有特性——延时精度等于 tick 间隔。

### 11.5 Demo: 同一系统内热切换 Tick 间隔

`demo_tick_rate.c` 展示两个场景在同一个 RTOS 实例中先后运行：

```
Phase 1: kernel_set_tick_interval_ms(500)
         → 环境监测模式, 每 500ms 采样一次
         → task_delay_ms(500) = 1 tick
         → 5 ticks ≈ 2.5 秒完成采样

Phase 2: kernel_set_tick_interval_ms(10)    ← 运行时热切换!
         → 小车巡线模式, 每 50ms 控制一次
         → task_delay_ms(50) = 5 ticks
         → 60 ticks ≈ 0.6 秒到达终点
```

运行：
```bash
make clean && make DEMO=demo_tick_rate && ./build/lite_rtos_demo
```

关键观察：
- Phase 1 中 tick 每 500ms 递增一次（慢速打印）
- Phase 2 中 tick 每 10ms 递增一次（快速打印）
- `task_delay_ms()` 在两种间隔下都传递相同的毫秒参数，行为一致
- 寄存器一致性验证全部通过 ✓

### 11.6 硬件层面：SysTick 的工作原理

在 ARM Cortex-M 上，整个 tick 机制就靠一个 24 位递减计数器：

```
每个 CPU 时钟周期:  SysTick->LOAD 减 1
减到 0 时:         触发 SysTick 异常 (中断)
                   硬件自动重装 LOAD 值
                   继续递减...

软件只做了一件事: 在初始化时写一次 LOAD 值。
之后完全由硬件自治运行，零软件开销。
```

```c
// lite_rtos_oncar/port.c (ARM 实现)
reload = (SystemCoreClock / g_tick_rate_hz) - 1;
SysTick->LOAD = reload;   // 一次寄存器写, <1ns
```

**SysTick 的限制**：24 位计数器最大值 = 16,777,215。在 480MHz STM32H743 上，单次最大间隔 ≈ 35ms。想设 500ms 间隔需要预分频器（8 分频后 = 60MHz → 最大 ~280ms），或者换用 32 位通用定时器。

### 11.7 Tick 间隔 vs 固定 Tick + 时间 API — 两种思路对比

本节顺带讨论一个常见的 RTOS 设计问题：

| | 可变 Tick 间隔 | 固定 Tick + 时间 API |
|---|---|---|
| 做法 | 改硬件定时器间隔 | 硬件固定高速 tick，软件换算时间 |
| 省电 | ✅ 低 tick 频率减少 ISR 次数 | ❌ ISR 始终高频 |
| `task_delay()` 语义 | 不稳定（取决于当前间隔） | 永远稳定 |
| 最低延时粒度 | 随间隔变化 | 固定（通常 1ms） |
| 适用场景 | 低功耗嵌入式设备 | 通用 RTOS |
| Lite RTOS 中 | `kernel_set_tick_interval_ms()` | `task_delay_ms()` |

**两者并存，各司其职**：`kernel_set_tick_interval_ms()` 管硬件（按需省电），`task_delay_ms()` 管软件（语义稳定）。这是本项目的设计选择。

---

---

## 12. Phase 10：任务数量上限控制

### 推荐先读这些文件

> `kernel.h` (MAX_TASKS, API 声明) → `kernel.c` (全局变量 + setter/getter) → `task.c` (task_create 中的检查)

### 12.1 为什么需要任务数量上限？

在之前的代码中，`kernel.h` 定义了 `#define MAX_TASKS 16`，但 `task_create()` **从未真正检查**这个值——它只是 `malloc` 然后加入就绪链表，对已创建的任务数毫无感知。这意味着在一个 buggy 的循环中，理论上可以创建成千上万个任务，耗尽所有堆内存。

解决方案分两层：
1. **编译期默认值**：`MAX_TASKS 16` 作为默认上限
2. **运行时可变**：通过 API 在程序启动后动态调整上限，无需重新编译

### 12.2 API 设计

```c
/* kernel.h — 运行时可变的任务数量上限 */

/* 设置最大任务数 (>=1, 已创建的任务不受影响) */
void     kernel_set_max_tasks(uint32_t max);

/* 获取当前最大任务数 */
uint32_t kernel_get_max_tasks(void);

/* 获取当前已创建的任务数 (不含 idle 任务) */
uint32_t task_get_count(void);
```

**设计原则**：
- `kernel_set_max_tasks()` 只约束**后续**的 `task_create()`，已创建的任务不受影响
- 下限保护：`max < 1` 时自动设为 1，至少允许一个用户任务
- idle 任务不计入 `g_task_count`——它是系统任务，始终存在

### 12.3 实现细节

**全局变量** ([kernel.c](kernel.c))：

```c
uint32_t  g_max_tasks  = MAX_TASKS;  /* 运行时最大任务数 (可通过 API 修改) */
uint32_t  g_task_count = 0;          /* 当前已创建的任务数 (不含 idle) */
```

**setter/getter** ([kernel.c](kernel.c))：

```c
void kernel_set_max_tasks(uint32_t max)
{
    if (max < 1) {
        max = 1;  /* 下限保护: 至少允许 1 个用户任务 */
    }
    g_max_tasks = max;
}

uint32_t kernel_get_max_tasks(void)
{
    return g_max_tasks;
}

uint32_t task_get_count(void)
{
    return g_task_count;
}
```

**task_create() 中的检查** ([task.c](task.c))：

```c
tcb_t *task_create(void (*task_func)(void),
                   const char *name,
                   uint32_t stack_size,
                   uint8_t priority)
{
    /* 参数检查 */
    if (task_func == NULL) { ... return NULL; }
    if (priority >= MAX_PRIORITIES) { ... return NULL; }

    /* ★ 新增: 任务数量上限检查 */
    if (g_task_count >= g_max_tasks) {
        fprintf(stderr, "task_create: task count %u reached limit %u\n",
                g_task_count, g_max_tasks);
        return NULL;
    }

    /* ... 分配 TCB、栈、初始化 context、加入就绪列表 ... */

    g_task_count++;  /* ★ 创建成功，递增计数 */

    return task;
}
```

**检查的插入位置**放在参数校验之后、`malloc` 之前——在分配任何内存之前就拒绝，避免半创建失败时的资源泄漏。

### 12.4 使用示例

```c
// 读取默认上限
printf("Max tasks: %u\n", kernel_get_max_tasks());  // 16

// 运行时设为仅允许 3 个任务
kernel_set_max_tasks(3);

tcb_t *t1 = task_create(func1, "task_a", 4096, 1);  // ✓ (1/3)
tcb_t *t2 = task_create(func2, "task_b", 4096, 1);  // ✓ (2/3)
tcb_t *t3 = task_create(func3, "task_c", 4096, 1);  // ✓ (3/3)
tcb_t *t4 = task_create(func4, "task_d", 4096, 1);  // ✗ 返回 NULL, 输出 "reached limit 3"

printf("Current tasks: %u\n", task_get_count());    // 3
```

### 12.5 ARM Cortex-M7 上任务上限的实际制约

`MAX_TASKS` 是软件限制，Cortex-M7 硬件本身不规定任务数量上限。但在实际嵌入式系统中，真正的制约来自以下因素：

#### ① SRAM (内存) — 主导因素

每个任务消耗的内存 = TCB 大小 + 栈大小。

| 组件 | 典型大小 |
|------|---------|
| TCB (`tcb_t`) | ~100–200 字节 |
| 任务栈 (简单任务) | 512 字节 – 2KB |
| 任务栈 (复杂任务) | 2KB – 8KB+ |

例如 STM32H743 (1MB SRAM) 上：
- 10 个任务 × 2KB 栈 + 10 × 200B TCB = **~22KB** — 非常宽裕
- 100 个任务 × 1KB 栈 + 100 × 200B TCB = **~120KB** — 仍有空间但接近极限

**在 RAM 耗尽之前，`MAX_TASKS` 就应该阻止新任务创建。** 一个创建泄漏的 bug（如循环中不断 `task_create`）在几秒内就会吃光 RAM。设置 `kernel_set_max_tasks(N)` 相当于给你的堆加上"熔断器"——不等 RAM 耗尽，内核先拒绝。

#### ② MPU 区域数 — 硬件硬限制

Cortex-M7 的 MPU (Memory Protection Unit) 通常有 **8 或 16 个区域**。如果使用 MPU 为每个任务做内存隔离（防止任务 A 访问任务 B 的栈），每个受保护任务占用一个 MPU 区域。此时 MPU 区域数就是绝对上限：

```
如果你有 8 个 MPU 区域，最多只能保护 8 个任务。
(实际上更少，因为系统区域、外设区域也会占用)
```

**不启用 MPU 则不受此限制。**

#### ③ 调度器时间复杂度

当前调度器 `find_highest_priority_task()` 的实现：

```c
for (int prio = 0; prio < MAX_PRIORITIES; prio++) {
    if (!list_is_empty(&ready_lists[prio])) {
        return list_entry(list_first(&ready_lists[prio]), tcb_t, node);
    }
}
```

时间复杂度 O(MAX_PRIORITIES) = O(8) ≈ O(1)，**与任务数量无关**。查找始终是扫描固定 8 个链表头。

但 `process_tick()` 中：
```c
list_for_each_safe(node, tmp, &delayed_list) {
    task->delay_ticks--;
    if (task->delay_ticks == 0) { /* 唤醒 */ }
}
```

时间复杂度 O(延时任务数)。如果 50 个任务同时延时，每个 tick 要遍历 50 次。在 480MHz M7 上，50 次遍历 + 递减 = 约 1μs，通常不构成瓶颈。

#### ④ 上下文切换开销

每次 `swapcontext` (或 ARM 上的 PendSV) 的耗时是**固定**的：

| 操作 | 典型耗时 (Cortex-M7 @ 480MHz) |
|------|-------------------------------|
| 保存 R4-R11 | ~200ns |
| 恢复 R4-R11 | ~200ns |
| 切换栈指针 (PSP) | ~5ns |
| **总计** | **~1–3μs** |

切换开销不随任务总数增加。但任务越多 → 调度越频繁 → 切换总次数增加。这是间接影响。

#### ⑤ 实际经验数据

FreeRTOS 在 Cortex-M7 上的典型应用：

| 应用类型 | 任务数 | 原因 |
|---------|--------|------|
| 简单传感器节点 | 3–5 | 采样、通信、管理 |
| 智能小车 | 5–10 | 巡线、避障、电机控制、通信、状态管理 |
| 工业控制器 | 10–20 | 多通道采样、多级控制、通信协议栈、安全监控 |
| 复杂 IoT 网关 | 15–30 | 协议栈任务、云通信、本地存储、多传感器融合 |
| 极端情况 | 30+ | 需要仔细审查栈使用量和 RAM 预算 |

**建议**：对一个典型的 M7 项目，`kernel_set_max_tasks(16)` 是合理起点。如果确实需要更多，先审查以下事项：

1. **SRAM 剩余**：计算 `Σ(栈大小ᵢ + sizeof(tcb_t))`，确保有 20%+ 余量
2. **栈使用量分析**：每个任务的栈到底用了多少？可以用栈填充模式 (`0xDEADBEEF`) 统计
3. **MPU 预算**：如果启用 MPU，留够区域数
4. **调度延迟分析**：用逻辑分析仪或 DWT 周期计数器测量最坏情况下的调度延迟

一句话总结：**SRAM 是你的实际上限，`MAX_TASKS` 是防止代码写坏的保护网。**

### 12.6 学习检查点

运行验证：
```bash
make clean && make DEMO=demo_task_create && ./build/lite_rtos_demo
```

应该看到：
```
[kernel] Task 'sensor' created: priority=1, stack=8192 bytes
[kernel] Task 'display' created: priority=3, stack=8192 bytes
[kernel] Task 'comm' created: priority=3, stack=8192 bytes
[kernel] Task 'storage' created: priority=5, stack=8192 bytes
[kernel] 已创建 4 个用户任务 + 1 个 idle 任务
```

尝试在代码中调用 `kernel_set_max_tasks(2)` 后创建更多任务，验证超限拒绝。

---

---

## 13. Phase 11：计数信号量 (Counting Semaphore)

### 推荐先读这些文件

> `sem.h` → `sem.c` → `demo_sem.c`

### 13.1 为什么需要信号量？

`task_delay()` 只能让任务**按时间**阻塞——"等我 50ms"。但 RTOS 中更常见的需求是**按条件**阻塞：

- "等红外传感器数据就绪"
- "等 IMU 加速度数据到达"
- "等电机控制计算完成"
- "等某个外设空闲"

这些场景的共同点是：**阻塞时长不确定，取决于另一个任务何时完成工作**。`task_delay()` 做不到这一点——你无法提前知道数据何时就绪。

信号量 (Semaphore) 是最基础的同步原语，解决的就是"任务间按条件等待"的问题。它由两个要素组成：

```
信号量 = 一个计数器 (count) + 一个等待队列 (wait_list)

  count > 0  →  资源可用，take 立即返回
  count == 0 →  资源被占满，take 的任务排队等待
  give 时    →  有排队者就唤醒队首，没有就 count++
```

### 13.2 信号量结构的内存布局

[sem.h](sem.h) 中的 `sem_t` 非常紧凑——只有 8 个字节（不算链表的哨兵节点）：

```c
typedef struct {
    int     count;       /* 可用资源计数 (>= 0) */
    list_t  wait_list;   /* 阻塞在此信号量上的任务队列 (FIFO) */
} sem_t;
```

其中 `list_t` 就是一个哨兵节点（`list_node_t sentinel`），含 `next` 和 `prev` 两个指针。信号量的全部工作就是在这条链表上增删任务的 `node` 成员：

```
信号量 sem_a_data (初始 count = 0):

  wait_list:
    ┌──────────────┐
    │   sentinel   │
    │ next ────────┼──→ 自己  (空链表)
    │ prev ────────┼──→ 自己
    └──────────────┘
  count = 0

当 Task B 调用 sem_take(&sem_a_data) 但 count == 0 时:

  wait_list:
    ┌──────────┐     ┌──────────────────────┐
    │ sentinel │←───→│  Task B 的 node 成员   │
    └──────────┘     │  (嵌入在 B 的 TCB 中)  │
                     └──────────────────────┘
  count = 0  (仍为 0，B 在排队)

当 Task A 调用 sem_give(&sem_a_data) 时:

  wait_list:
    ┌──────────┐
    │ sentinel │  (恢复为空)
    └──────────┘
  count = 0  (令牌直接转交给 B，count 不变)

  Task B 已通过 task_add_to_ready() 回到 ready_lists[B.priority]。
```

这和 FreeRTOS 的 `xSemaphore` 结构完全一致：侵入式链表串起所有等待者，节点嵌入在 TCB 内部，不额外分配内存。

### 13.3 全部 API

```c
/* kernel.h → sem.h — 计数信号量 API */

void sem_init(sem_t *sem, int initial_count);   /* 初始化信号量, 设 count 初值 */
void sem_take(sem_t *sem);                      /* 阻塞获取: count-- 或排队等待 */
int  sem_try_take(sem_t *sem);                  /* 非阻塞尝试: 成功返回 1, 失败返回 0 */
void sem_give(sem_t *sem);                      /* 释放 (任务上下文), 可能触发调度 */
void sem_give_from_isr(sem_t *sem);             /* 释放 (ISR 上下文), 不调 schedule */
```

5 个 API，总计约 90 行实现代码。核心只有两个函数：`sem_take`（获取）和 `sem_give`（释放），其余是辅助。

### 13.4 `sem_init()` — 初始化

```c
void sem_init(sem_t *sem, int initial_count)
{
    sem->count = initial_count;
    list_init(&sem->wait_list);  /* sentinel.next = sentinel.prev = &sentinel */
}
```

在调度器启动**之前**调用（通常紧接 `kernel_init()` 之后）。此时没有中断、没有并发，无需临界区保护。

`initial_count` 的含义：

| initial_count | 语义 | 典型用途 |
|---------------|------|----------|
| 0 | 资源初始不可用，消费者需等待生产者 | 数据就绪通知 |
| 1 | 二值信号量 (Binary Semaphore)，互斥 | 保护共享资源 |
| N (>1) | 计数信号量，N 份相同资源可用 | 管理缓冲池、外设通道 |

Demo 中使用 `sem_init(&sem_a_data, 0)` — 初始无数据，消费者必须等待生产者先 `give`。

### 13.5 `sem_take()` — 阻塞获取（逐行详解）

[sem.c](sem.c) 的 `sem_take()` 是整个信号量模块最核心的函数。它有两条完全不同的执行路径，取决于 `count` 的值：

```c
01: void sem_take(sem_t *sem)
02: {
03:     port_disable_interrupts();           // ← ① 临界区开始
04:
05:     /* ═══════════ 快路径: 资源可用 ═══════════ */
06:     if (sem->count > 0) {
07:         sem->count--;                    // ← ② 递减 count, 取走令牌
08:         port_enable_interrupts();        // ← ③ 临界区结束
09:         return;                          // ← ④ 立即返回, 不阻塞!
10:     }
11:
12:     /* ═══════════ 慢路径: 资源不可用, 阻塞 ═══════════ */
13:     task_remove_from_ready(current_task);  // ← ⑤ 离开就绪链表
14:     current_task->state = TASK_BLOCKED;    // ← ⑥ 标记阻塞态
15:     list_insert_tail(&sem->wait_list,      // ← ⑦ 挂到信号量等待队列
16:                      &current_task->node);
17:
18:     port_enable_interrupts();              // ← ⑧ 临界区结束
19:     schedule();                            // ← ⑨ 让出 CPU!
20:
21:     /* ★ 被 sem_give 唤醒后从此处继续执行 ★
22:      * 令牌已由 sem_give 直接转移给本任务,
23:      * 无需递减 count — 直接 return 即可。 */
24: }
```

#### 逐行解读

**第 3 行 — `port_disable_interrupts()`**：进入临界区。在修改 `count` 和操作链表的整个过程中，必须屏蔽中断（Unix 上是 SIGALRM），防止 tick ISR 在操作中途插入并破坏数据结构。

**第 6-10 行 — 快路径**：如果 `count > 0`（资源池里有令牌），直接拿走一个令牌（`count--`），开中断，返回。整条路径零阻塞、零上下文切换。这是最常见的路径——生产者已经 `give` 过，消费者来了直接取走。

**第 13 行 — `task_remove_from_ready(current_task)`**：把当前任务从 `ready_lists[priority]` 中摘除。此时 `current_task` 不再是就绪态，调度器不会再选中它。函数实现：

```c
void task_remove_from_ready(tcb_t *task) {
    if (task == NULL) return;
    list_remove(&task->node);  // O(1) 删除, 不需要知道属于哪个链表
}
```

**第 14 行 — `current_task->state = TASK_BLOCKED`**：标记为阻塞态。`schedule_select_and_switch()` 中有一个关键判断：如果 `prev->state == TASK_RUNNING` 则改为 `TASK_READY`。但 `TASK_BLOCKED` 不会被改回 `READY`——这确保了任务直到被显式唤醒才可能被调度。

**第 15-16 行 — `list_insert_tail()`**：把当前任务的 `node` 成员插入信号量等待队列的尾部（FIFO）。这是一个纯指针操作——修改 `node->prev`、`node->next` 以及邻居节点的指针，不涉及任何内存分配。

**第 19 行 — `schedule()`**：这是关键一步——主动让出 CPU。调度器会：
1. 再次关中断 → 处理 tick（如有）
2. 调用 `find_highest_priority_task()` 扫描就绪链表
3. 找到最高优先级的就绪任务（当前任务已不在任何就绪链表中）
4. 通过 `swapcontext()` 切换到那个任务

当前任务在此"冻结"——它的执行上下文（RSP、RIP、所有寄存器）被保存在 `current_task->context` 中，下次被唤醒时从第 21 行继续。

**第 21 行以后 — 被唤醒后的继续点**：当另一个任务调用 `sem_give()` 并从 `wait_list` 中弹出当前任务、调用 `task_add_to_ready()` 将它放回就绪链表，最终调度器选中它时，执行从 `schedule()` 调用之后继续——就像 `schedule()` 只是一个"耗时很长的函数"。

#### 令牌转移机制

这是信号量设计中最精妙的部分。被唤醒后**为什么不需要递减 `count`**？

因为令牌的转移发生在 `sem_give` 端：

```
场景 A: sem_give 时无人等待
  sem_give() → count++                   (令牌暂存在 count 里)
  sem_take() → count-- → return          (后来者从 count 取走令牌)

场景 B: sem_give 时有人等待
  sem_give() → pop wait_list → task_add_to_ready  (令牌直接转交)
  sem_take() → 被唤醒, 直接 return                (令牌已在自己手里)

关键: 场景 B 中 count 始终为 0, 令牌从未经过 count 字段。
      "唤醒"这个动作本身就是令牌的转移。
```

如果在被唤醒后再递减 `count`，就相当于**拿了两个令牌**——一个通过"被唤醒"拿到，一个通过"递减 count"拿到。而 `count` 可能已经是 0（场景 B），减去后变成 -1，彻底破坏了信号量的不变量（`count >= 0`）。

### 13.6 `sem_give()` — 释放信号量（逐行详解）

```c
01: void sem_give(sem_t *sem)
02: {
03:     tcb_t *woken = NULL;                 // ← ① 记录被唤醒的任务
04:
05:     port_disable_interrupts();           // ← ② 临界区开始
06:
07:     if (!list_is_empty(&sem->wait_list)) {
08:         /* ═════ 有等待者: 唤醒队首 (FIFO) ═════ */
09:         list_node_t *node = list_pop_head(&sem->wait_list);
10:                                          // ← ③ 弹出队首, O(1)
11:         woken = list_entry(node, tcb_t, node);
12:                                          // ← ④ 反查 TCB 指针
13:         task_add_to_ready(woken);        // ← ⑤ 放回就绪链表
14:     } else {
15:         /* ═════ 无等待者: count++ ═════ */
16:         sem->count++;                    // ← ⑥ 令牌暂存
17:     }
18:
19:     port_enable_interrupts();            // ← ⑦ 临界区结束
20:
21:     /* ★ 仅当唤醒任务优先级更高时才触发调度 ★ */
22:     if (woken != NULL && woken->priority < current_task->priority) {
23:         schedule();                      // ← ⑧ 条件调度
24:     }
25: }
```

#### 逐行解读

**第 3 行 — `woken = NULL`**：这是关键变量，记录了"本次 give 是否唤醒了一个任务"。初始为 `NULL`，只在有等待者时被赋值。

**第 7 行 — `list_is_empty(&sem->wait_list)`**：判断等待队列是否有任务在排队。这是一个 O(1) 操作——只比较 `sentinel.next == &sentinel`。

**第 9-13 行 — 有等待者路径**：

- `list_pop_head()` 做了两件事：取出队首节点 + 把它从链表中移除。O(1)。
- `list_entry(node, tcb_t, node)` 用 `container_of` 宏从 `node` 的地址减去 `node` 在 `tcb_t` 中的偏移量，得到整个 TCB 的指针。详见 §3.2。
- `task_add_to_ready(woken)` 做了三件事：
  1. `woken->state = TASK_READY` — 改状态
  2. `list_insert_tail(&ready_lists[woken->priority], &woken->node)` — 挂回就绪链表
  3. 此后调度器的 `find_highest_priority_task()` 就能看到这个任务了

**第 14-16 行 — 无等待者路径**：简单递增 `count`。未来某个任务调用 `sem_take()` 时走快路径直接取走。

**第 22-24 行 — 条件调度**：这是 Lite RTOS 信号量实现的一个关键设计决策。`schedule()` 不是无条件调用的。

```c
if (woken != NULL && woken->priority < current_task->priority) {
    schedule();
}
```

条件有二：① 确实唤醒了一个任务；② 被唤醒任务的优先级**数值上小于**（即优先级更高）当前任务。两个条件同时满足才触发调度。

**为什么加这个限制？** 考虑 Demo 中的场景：

```
Task A "IR"  (prio 0, 最高) 调用 sem_give()
  → 唤醒 Task B "Obst" (prio 2)
  → 如果立即 schedule(), 切到 B (prio 2)
  → 但 A (prio 0) 比 B 更紧急! A 不应该被 B 抢占!
```

不加优先级检查的话，高优先级任务每 `give` 一次就被切走——即使切向的是一个更低优先级的任务。这既不合理（违反优先级调度原则），也低效（额外的上下文切换开销）。

加了检查后：A(prio 0) 给 B(prio 2) 发信号 → 不切 → A 继续执行完当前周期 → A 最后调用 `task_delay_ms(50)` → `schedule()` → 此时 A 已阻塞，B 成为最高就绪任务 → 切到 B。

反之，如果一个**低优先级任务**（如 prio 4）give 给一个**高优先级任务**（如 prio 0）：

```
Task C "Motor" (prio 3) 调用 sem_give()
  → 唤醒 Task A "IR" (prio 0)
  → woken->priority (0) < current_task->priority (3)  ✅ 满足!
  → schedule() → 立即切换到 A!
  → A 是高优任务, 应该立刻运行 ✔
```

这就是"条件调度"的精髓：**只有更高优先级的任务被唤醒时才值得立即切换。**

### 13.7 `sem_try_take()` — 非阻塞获取

```c
int sem_try_take(sem_t *sem)
{
    port_disable_interrupts();

    if (sem->count > 0) {
        sem->count--;                 /* 令牌到手 */
        port_enable_interrupts();
        return 1;                     /* 成功 */
    }

    port_enable_interrupts();
    return 0;                         /* 资源不可用, 不阻塞 */
}
```

和 `sem_take` 的区别只有一点：**没有慢路径**。`count == 0` 时不排队、不阻塞，直接返回 0。调用者自行决定是放弃、重试还是做别的。

典型用法：

```c
/* 检查数据是否就绪, 不就绪则跳过本轮处理 */
if (sem_try_take(&sem_data)) {
    process_data();
} else {
    /* 数据还没好, 先干别的 */
    do_other_work();
}
```

### 13.8 `sem_give_from_isr()` — ISR 上下文释放

```c
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
    /* ★ 不调用 schedule() — 由 ISR 框架统一调度 */
}
```

和 `sem_give` 的唯一区别：**末尾没有 `schedule()` 调用**。

原因在于调用上下文不同。`sem_give_from_isr` 预期在中断服务程序 (ISR) 中调用。在 Unix 平台上，SIGALRM 信号处理函数调用链是：

```
SIGALRM 触发
  → tick_signal_handler()
    → [用户代码可能调用 sem_give_from_isr()]
    → schedule_from_isr()
      → process_tick()            // 处理延时到期
      → schedule_select_and_switch()  // ★ 这里统一调度!
```

如果 `sem_give_from_isr` 自己调了 `schedule()`，会发生**双重调度**——先在这里切一次，回到 ISR 框架后又切一次。不仅浪费，还可能导致状态错乱。

所以 `sem_give_from_isr` 只负责"把任务放回就绪链表"，把"选谁运行"的决定留给 ISR 出口的 `schedule_from_isr()`。这是 FreeRTOS 中 `xSemaphoreGiveFromISR` 的标准模式。

### 13.9 深入理解：令牌转移的三个时序

理解信号量的关键是把 `count` 和 `wait_list` 看作**令牌的两种存储形式**：

```
count 是"暂存区" — 令牌暂时存在计数器里, 等下一次 take 取走
wait_list 是"直达通道" — 令牌不经过 count, 直接交给等待者
```

以下是三个典型时序，覆盖所有可能的情况：

**时序 1: 先生产后消费（count 暂存）**

```
时间 ──────────────────────────────────────────→

Task A (生产者):    [init count=0]    sem_give()     ...
                                         │
                                    count 变成 1

Task B (消费者):    ...                  (sleep)     sem_take()
                                                       │
                                                   count>0 → count=0
                                                   快路径, 立即返回
                                                   令牌从 count 转给 B
```

**时序 2: 先消费后生产（wait_list 直达）**

```
时间 ──────────────────────────────────────────→

Task B (消费者):    sem_take()    ...        (blocked)
                       │
                  count==0 → 加入 wait_list
                  让出 CPU

Task A (生产者):    ...          sem_give()
                                    │
                              wait_list 非空 → pop B
                              task_add_to_ready(B)
                              令牌直达 B, count 仍为 0
```

**时序 3: 多消费者排队（FIFO 唤醒）**

```
Task B (prio 2) 和 Task C (prio 3) 都等待 sem_a_data:

wait_list:  sentinel ←→ [B.node] ←→ [C.node] ←→ sentinel
                         队首 ↑        队尾 ↑

Task A 第一次 sem_give():
  → list_pop_head() 弹出 B  ← FIFO! B 先到先得
  → task_add_to_ready(B)     ← B 回到 ready_lists[2]
  → B.priority(2) > A.priority(0)? 否 → 不切

Task A 第二次 sem_give():
  → list_pop_head() 弹出 C  ← C 还在排队
  → task_add_to_ready(C)     ← C 回到 ready_lists[3]
  → 不切

Task A 最后 task_delay():
  → schedule()
  → find_highest_priority_task() 从 prio=0 扫描:
      ready_lists[0] = 空 → 继续
      ready_lists[1] = 空 → 继续
      ready_lists[2] = [B] → 返回 B!
  → swapcontext(A, B)  ← B 先运行 (优先级更高)
  
B 运行完 → task_delay → schedule() → 选 C (prio 3) → swapcontext(B, C)
```

注意：B 和 C 虽然都在 `ready_lists` 中，但**不是按 FIFO 顺序运行的**——一旦回到就绪链表，调度器的优先级扫描机制接管，B(prio 2) 优先于 C(prio 3)。信号量的 FIFO 只控制"谁先从等待队列被唤醒"，不管"唤醒后谁先运行"。

### 13.10 Demo 全流程追踪

以 [demo_sem.c](demo_sem.c) 的第一周期为例，追踪从启动到所有任务完成一次循环的完整过程。tick 间隔为 10ms。

**阶段 0: 启动 (tick 0)**

```
kernel_start() → 选最高优先级就绪任务: IR (prio 0) → 切换到 IR

IR 开始执行:    task_delay(1) → 阻塞 1 tick → 切到 IMU (prio 1)
IMU 开始执行:   task_delay(1) → 阻塞 1 tick → 切到 Obst (prio 2)  
Obst 开始执行:  sem_take(&sem_a_data) → count==0 → 阻塞, 加入 wait_list
Motor 开始执行: sem_take(&sem_a_data) → count==0 → 阻塞, 加入 wait_list

所有 4 个任务都阻塞! idle 任务运行。
```

此时链表快照：

```
ready_lists[0]: (空)
ready_lists[1]: (空)  
ready_lists[2]: (空)
ready_lists[3]: (空)
ready_lists[7]: [idle]                    ← 唯一就绪的任务

delayed_list:   sentinel ←→ [IR.node(delay=1)] ←→ [IMU.node(delay=1)]

sem_a_data.wait_list: sentinel ←→ [Obst.node] ←→ [Motor.node]
sem_d_data.wait_list: (空)
```

**阶段 1: IR 苏醒, 生产数据 (tick 1-3)**

```
tick 1: process_tick() → IR.delay_ticks 归零 → task_add_to_ready(IR)
        schedule_select_and_switch() → IR (prio 0) vs idle (prio 7) → 切到 IR

IR:     [IR] step 2/3: reading ADC... → task_delay(1)

tick 2: IR 又醒了 → [IR] step 3/3: computing distance...
        → task_delay(1)

tick 3: IR 又醒了 → [IR] ★ PRODUCED: ir_distance = 150
        sem_give(&sem_a_data)  → 弹出 Obst, task_add_to_ready(Obst)
        sem_give(&sem_a_data)  → 弹出 Motor, task_add_to_ready(Motor)
        task_delay_ms(50)      → IR 休眠 50ms, schedule()!
```

调度器扫描：`ready_lists[0]=空, [1]=空, [2]=[Obst]` → 切到 Obst！

**阶段 2: Obst 消费, 分析障碍物 (tick 3-5)**

```
Obst:   [Obst] got ir_distance=150, analyzing...
        [Obst] step 1/2: threshold analysis... → task_delay(1)
        
tick 4: Obst 醒了 → [Obst] step 2/2: decision making... → task_delay(1)

tick 5: Obst 醒了 → [Obst] ★ RESULT: clear (dist=150 >= 100)
        task_delay_ms(20) → Obst 休眠 20ms → schedule()!
```

调度器扫描：`ready_lists[0]=空, [1]=空, [2]=空 (Obst 刚离开), [3]=[Motor]` → 切到 Motor！

**阶段 3: Motor 第一次等待, IMU 第二次等待 (tick 5-6)**

```
Motor:  sem_take(&sem_a_data) → 快路径! count 还有令牌吗?
```

等等——A 给了两次 `sem_give`，第一次弹出 Obst（令牌直达），第二次弹出 Motor（令牌也直达）。Motor 已经不在 `sem_a_data.wait_list` 里了，它是在 `ready_lists[3]` 中被调度器选中的。Motor 在 `sem_take` 调用时，`count` 是 0，`wait_list` 是空的——两个 `sem_give` 都直接唤醒了等待者，没有经过 `count`。

```
Motor:  sem_take(&sem_a_data) → count==0 → 但 Motor 已经在 ready_lists 里了?
```

**不对**——让我重新追踪。Obst 和 Motor 都在 tick 0 调用了 `sem_take(&sem_a_data)`，都进了 `wait_list`。然后在 tick 3，A 调用了两次 `sem_give`：

- 第一次 `sem_give`：`list_pop_head` 弹出 Obst → `task_add_to_ready(Obst)` → Obst 在 `ready_lists[2]`
- 第二次 `sem_give`：`list_pop_head` 弹出 Motor → `task_add_to_ready(Motor)` → Motor 在 `ready_lists[3]`

Obst 先运行（prio 2 > prio 3）。Obst 做完了自己的分析工作后 `task_delay_ms(20)`，调度器切到 Motor。Motor 已经在 `ready_lists[3]` 中，它之前调用的 `sem_take` 已经返回了（在 `schedule()` 之后继续执行）。所以 Motor 现在继续往下走：

```
Motor:  [Motor] got ir_distance=150, now waiting for IMU...
        sem_take(&sem_d_data) → count==0, wait_list 空
        → 这次真的要阻塞了! 加入 sem_d_data.wait_list → schedule()
```

调度器扫描：所有用户任务都阻塞了（IR 延时、IMU 延时、Obst 延时、Motor 在信号量等待）→ 切到 idle。

**阶段 4: IMU 苏醒, 生产数据, Motor 被唤醒 (tick 6-8)**

```
tick 6: idle 运行...

tick 7: process_tick() → IMU.delay_ticks 归零 → task_add_to_ready(IMU)
        schedule_select_and_switch() → IMU (prio 1) → 切到 IMU

IMU:    [IMU] ★ PRODUCED: accel=(+0.75, -0.02, +1.00)
        sem_give(&sem_d_data) → wait_list 非空 → pop Motor!
        → task_add_to_ready(Motor)
        → Motor.priority(3) > IMU.priority(1)? 是，3>1 → 不切
        task_delay_ms(70) → IMU 休眠 → schedule()

调度器: ready_lists[3]=[Motor] → 切到 Motor!
```

**阶段 5: Motor 完成控制计算 (tick 8-11)**

```
Motor:  sem_take(&sem_d_data) 返回 (被 IMU 唤醒后从此处继续!)
        [Motor] got IMU (ax=0.75, ay=-0.02), computing...
        [Motor] step 1/3: sensor fusion... → task_delay(1)
        
tick 9: [Motor] step 2/3: PID computation... → task_delay(1)
tick 10:[Motor] step 3/3: PWM output... → task_delay(1)
tick 11:[Motor] ★ COMMAND: speed=80, steer=-0.3, PWM=(L:80, R:80)
        task_delay_ms(30) → Motor 休眠

第一周期完成! ✅
```

**关键观察总结**：

| tick | 事件 | 阻塞机制 |
|------|------|----------|
| 0 | 四任务全部阻塞 | `sem_take` (Obst, Motor) + `task_delay` (IR, IMU) |
| 1-3 | IR 生产数据, give ×2 | 两个 `sem_give` 唤醒 Obst 和 Motor |
| 3-5 | Obst 分析, Motor 等 D | Obst 运行完; Motor 在 `sem_take(&sem_d_data)` 阻塞 |
| 7-8 | IMU 生产数据, give | `sem_give` 唤醒 Motor |
| 8-11 | Motor 完成计算 | 双信号量依赖满足, 输出控制指令 |

### 13.11 与 `task_delay` 阻塞路径的完整对比

```
                    task_delay(N)               sem_take(sem)
                    ────────────                ─────────────
脱离就绪:           task_remove_from_ready      完全相同
标记状态:           state = TASK_BLOCKED        完全相同
等待队列:           全局 delayed_list           信号量私有的 wait_list
附加信息:           delay_ticks = N             无
让出 CPU:           schedule()                  完全相同
唤醒者:             process_tick()              sem_give()
唤醒操作:           list_remove + task_add      list_pop_head + task_add (更深奥)
唤醒触发条件:       delay_ticks 减到 0          另一个任务调用 sem_give()
```

本质区别只有一个：**等时间 vs 等事件**。但内核的阻塞/唤醒基础设施是同一套代码。

### 13.12 设计边界与限制

**单节点约束**：TCB 只有一个 `list_node_t node`，调用 `sem_take` 慢路径时从 `ready_lists` 移入 `sem->wait_list`；调用 `task_delay` 时则需要移入 `delayed_list`。任务不能同时在两个等待队列中。这意味着**不支持带超时的信号量等待**——语义是"等信号量，最多等 N 个 tick"。要支持需要给 TCB 增加第二个 `list_node_t`。

**FIFO 唤醒顺序**：`list_pop_head` 弹出最早排队的任务，不按优先级重排 `wait_list`。这保持简单，且在大多数场景下足够。FreeRTOS 默认也是 FIFO。

**ISR 限制**：`sem_give_from_isr` 不调用 `schedule()`，要求在 ISR 出口处有统一的调度点。当前 SIGALRM 处理函数中的 `schedule_from_isr()` 满足此要求。如果在其他 ISR（如 GPIO）中使用，确保 ISR 框架包含 `schedule_from_isr()` 调用。

**计数不能为负**：`sem_take` 的慢路径不递减 `count`（因为 `count` 已经是 0），`sem_give` 在无等待者时递增 `count`。`count` 始终 ≥ 0，这是代码不变量。

---

## 14. 与 FreeRTOS 的对比

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
| 系统节拍 | SIGALRM (可设 1~1000ms) | SysTick (通常 1000Hz) |
| Tick 间隔可配 | ✅ `kernel_set_tick_interval_ms()` | ❌ 仅编译期 `configTICK_RATE_HZ` |
| `task_delay_ms()` | ✅ | ✅ `vTaskDelay(pdMS_TO_TICKS(ms))` |
| 优先级位图 | ❌ (遍历 8 个优先级) | ✅ `uxTopReadyPriority` (O(1)) |
| 时间片轮转 | ❌ | ✅ (同优先级按时间片) |
| 互斥量 | ❌ | ✅ |
| 计数信号量 | ✅ `sem_t` (Phase 11) | ✅ |
| 消息队列 | ❌ | ✅ |
| 内存管理 | malloc/free | heap_1~heap_5 |

---

## 15. 扩展方向

### 15.1 优先级位图加速

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

### 15.2 时间片轮转

同优先级任务各执行 1 个 tick，然后自动切换到同优先级的下一个：

```c
// 在 process_tick() 中：
if (当前任务优先级有其他就绪任务 && 时间片用完) {
    task_remove_from_ready(current_task);
    task_add_to_ready(current_task);  // 移到链表尾部
    schedule();  // 触发同优先级切换
}
```

### 15.3 消息队列 (Message Queue)

信号量已作为 Phase 11 正式实现（见 §13），此处预留给下一阶段的消息队列：

```c
typedef struct {
    void   *buffer;       // 消息缓冲区
    size_t  item_size;    // 每条消息大小
    size_t  max_items;    // 最大消息数
    size_t  read_idx;     // 读指针
    size_t  write_idx;    // 写指针
    sem_t   sem_rx;       // 接收信号量 (数据就绪通知)
    sem_t   sem_tx;       // 发送信号量 (缓冲区空闲通知)
} mqueue_t;
```

### 15.4 ARM Cortex-M3 移植

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

### 15.5 调试增强

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
| 11 | `demo_tick_rate.c` | 看可变 tick 间隔 + 毫秒级延时（Phase 9） |
| 12 | `sem.h` | 信号量类型与 API 声明（Phase 11） |
| 13 | `sem.c` | 信号量实现——`sem_take` 复用 `task_delay` 的阻塞模式 |
| 14 | `demo_sem.c` | 看计数信号量如何同步多任务数据流（Phase 11） |
| 15 | 全部重读一遍 | 第二次阅读会有全新的理解 |

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
15. **`task_delay()` 和 `task_delay_ms()` 的区别？** → `task_delay(N)` 的参数是 tick 数，实际时间取决于 tick 间隔；`task_delay_ms(N)` 的参数是真实毫秒，与 tick 间隔解耦。推荐用后者。详见 §7.4b
16. **为什么不用两个硬件定时器来实现"快慢两种 tick"？** → 不需要。固定一个高速 tick + `task_delay_ms()` 就能覆盖所有场景。相机等慢任务阻塞时 CPU 进入 WFI 睡眠，比降低 tick 频率更省电。两个定时器会显著增加复杂度且 PC 模拟器不支持。详见 §11.7
