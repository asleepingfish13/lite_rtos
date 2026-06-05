/**
 * @file    port.c
 * @brief   Lite RTOS - 平台移植层实现 (跨平台)
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │  平台         │  上下文切换    │  系统节拍                  │
 * ├─────────────────────────────────────────────────────────────┤
 * │  Unix         │  ucontext      │  setitimer + SIGALRM       │
 * │  (macOS/Linux)│  swapcontext() │  信号处理函数设置标志位     │
 * ├─────────────────────────────────────────────────────────────┤
 * │  Windows      │  Fibers API    │  后台线程 + Sleep(10ms)    │
 * │  (MSYS2)      │  SwitchToFiber│  线程设置标志位             │
 * └─────────────────────────────────────────────────────────────┘
 *
 * 设计原理 (两个平台统一):
 *   上下文切换 = 保存当前"执行现场" → 切换到另一个任务的"执行现场"
 *   系统节拍   = 定时器 → 设置 tick_pending 标志 → schedule() 处理
 *   临界区     = 保护内核数据结构不被"中断"破坏
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    /* Windows: Fibers + 后台线程 */
    #include <windows.h>
    #include <process.h>
#else
    /* Unix: ucontext + 信号 */
    #include <signal.h>
    #include <sys/time.h>
    #include <unistd.h>
#endif

#include "port.h"
#include "task.h"

/* 外部声明 (kernel.c) */
extern void     schedule(void);
extern void     schedule_from_isr(void);
extern uint32_t tick_count;
extern uint8_t  scheduler_running;
extern tcb_t   *current_task;

/* ====================================================================
 *  系统节拍配置
 * ==================================================================== */
static uint32_t g_tick_rate_hz = 100;  /* 默认 100Hz (10ms/tick)，可通过 kernel_set_tick_rate() 修改 */

/* ====================================================================
 *  信号安全标志 (跨平台共用)
 * ==================================================================== */
static volatile int tick_pending = 0;

/* ====================================================================
 *  Windows 实现: Fibers API + 后台 tick 线程
 *
 *  注意: 真抢占 (ISR 驱动的上下文切换) 目前仅在 Unix 上通过
 *  SIGALRM 实现。在 Windows 上，tick 线程运行在独立的 OS 线程中，
 *  无法直接调用 SwitchToFiber() 操作主线程的 fiber。
 *  Windows 上的抢占依赖 idle 任务的 tick_pending 轮询
 *  和 task_delay()/task_yield() 调度点 (延迟抢占模式)。
 * ==================================================================== */
#ifdef _WIN32

/* -------------------------------------------------------------------
 *  Windows 上下文切换 (Fibers API)
 *
 *  Fiber 是 Windows 的用户态协程:
 *    ConvertThreadToFiber() — 把当前线程转成 fiber
 *    CreateFiber()          — 创建一个新 fiber (指定栈大小和入口函数)
 *    SwitchToFiber()        — 切换到目标 fiber (隐式保存当前 fiber 状态)
 *    DeleteFiber()          — 销毁 fiber
 *
 *  与 ucontext 的对应关系:
 *    makecontext()  ≈ CreateFiber()
 *    swapcontext()  ≈ SwitchToFiber()
 *    setcontext()   ≈ SwitchToFiber() (从 ConvertThreadToFiber 的结果)
 * ------------------------------------------------------------------- */

void port_init_task_context(tcb_t *task, void (*task_func)(void))
{
    task->fiber = CreateFiber(task->stack_size,
                               (LPFIBER_START_ROUTINE)task_func,
                               NULL);
    if (task->fiber == NULL) {
        fprintf(stderr, "[port] CreateFiber failed: error %lu\n", GetLastError());
        exit(1);
    }
}

void port_start_first_task(tcb_t *task)
{
    printf("[port] Starting first task: '%s'\n", task->name);
    /* 把当前线程 (main) 转成 fiber, 这是使用其他 fiber 的前提 */
    ConvertThreadToFiber(NULL);
    /* 跳转到第一个任务, 不保存当前 fiber (一去不回) */
    SwitchToFiber(task->fiber);
    /* 不会执行到这里 */
    perror("SwitchToFiber");
    exit(1);
}

void port_switch_context(tcb_t *from, tcb_t *to)
{
    (void)from;  /* from 的状态由 SwitchToFiber 隐式保存 */
    SwitchToFiber(to->fiber);
    /* 当 from 再次被调度时, 从这里继续执行 */
}

/* -------------------------------------------------------------------
 *  Windows 系统节拍 (后台线程)
 *
 *  创建一个独立的线程, 每 10ms 设置 tick_pending 标志。
 *  与 Unix 信号处理函数的作用完全等价。
 *
 *  注意: 这里不需要处理"抢占"。tick 线程只设置标志,
 *        实际的延时任务处理 + 调度在 schedule() 中完成。
 * ------------------------------------------------------------------- */
static HANDLE tick_thread_handle = NULL;
static volatile int tick_thread_running = 1;

static DWORD WINAPI tick_thread_func(LPVOID param)
{
    (void)param;
    while (tick_thread_running) {
        DWORD sleep_ms = 1000 / g_tick_rate_hz;
        if (sleep_ms < 1) sleep_ms = 1;  /* 防止 0ms Sleep (忙等) */
        Sleep(sleep_ms);
        tick_pending = 1;
    }
    return 0;
}

void port_init_tick_timer(void)
{
    tick_thread_handle = CreateThread(
        NULL,                /* 默认安全属性 */
        0,                   /* 默认栈大小 */
        tick_thread_func,    /* 线程函数 */
        NULL,                /* 参数 */
        0,                   /* 创建标志 */
        NULL                 /* 线程 ID (不需要) */
    );

    if (tick_thread_handle == NULL) {
        fprintf(stderr, "[port] CreateThread failed: error %lu\n", GetLastError());
        exit(1);
    }

    printf("[port] Tick thread started: %u Hz (%lu ms/tick)\n",
           g_tick_rate_hz, (unsigned long)(1000 / g_tick_rate_hz));
}

/* -------------------------------------------------------------------
 *  Windows 临界区
 *
 *  Windows 上没有"中断"的概念。tick 线程和主线程之间通过
 *  tick_pending 标志通信, 该标志的读写是原子的 (int 在 x86/ARM 上)。
 *  链表操作只在主线程 (schedule) 中进行, 无需额外保护。
 *
 *  为了 API 兼容性, 提供空实现。
 * ------------------------------------------------------------------- */
void port_disable_interrupts(void) { /* Windows: no-op */ }
void port_enable_interrupts(void)  { /* Windows: no-op */ }

/* ====================================================================
 *  Unix 实现 (macOS / Linux)
 * ==================================================================== */
#else

/* -------------------------------------------------------------------
 *  Unix 上下文切换 (ucontext)
 * ------------------------------------------------------------------- */

void port_init_task_context(tcb_t *task, void (*task_func)(void))
{
    if (getcontext(&task->context) == -1) {
        perror("getcontext");
        exit(1);
    }

    task->context.uc_stack.ss_sp    = task->stack;
    task->context.uc_stack.ss_size  = task->stack_size;
    task->context.uc_stack.ss_flags = 0;
    task->context.uc_link = NULL;

    makecontext(&task->context, (void (*)(void))task_func, 0);
}

void port_start_first_task(tcb_t *task)
{
    printf("[port] Starting first task: '%s'\n", task->name);
    setcontext(&task->context);
    perror("setcontext");
    exit(1);
}

void port_switch_context(tcb_t *from, tcb_t *to)
{
    if (swapcontext(&from->context, &to->context) == -1) {
        perror("swapcontext");
        exit(1);
    }
}

/* -------------------------------------------------------------------
 *  Unix 系统节拍 (setitimer + SIGALRM)  —  真抢占模式
 *
 *  SIGALRM 处理函数在信号上下文中运行，
 *  直接调用 schedule_from_isr() 完成 tick 处理和抢占调度。
 *
 *  schedule_from_isr() 内部的代码路径遵守信号安全规则:
 *    ✓ sig_atomic_t / volatile int 读写
 *    ✓ sigprocmask (POSIX 异步信号安全)
 *    ✓ 纯指针操作 (链表遍历/修改)
 *    ✓ swapcontext (实践中在 macOS/Linux 上可从信号处理器调用)
 *    ✗ 不调用 printf/malloc 等
 * ------------------------------------------------------------------- */

static void tick_signal_handler(int sig)
{
    (void)sig;
    tick_pending = 1;
    schedule_from_isr();
    /* 当被抢占的任务恢复时，从此处继续执行，
     * 然后返回信号处理函数 → sigreturn 恢复任务原始上下文。 */
}

void port_init_tick_timer(void)
{
    struct sigaction sa;
    struct itimerval timer;
    uint32_t interval_us = 1000000 / g_tick_rate_hz;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tick_signal_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    timer.it_value.tv_sec     = interval_us / 1000000;
    timer.it_value.tv_usec    = interval_us % 1000000;
    timer.it_interval.tv_sec  = interval_us / 1000000;
    timer.it_interval.tv_usec = interval_us % 1000000;

    if (setitimer(ITIMER_REAL, &timer, NULL) == -1) {
        perror("setitimer");
        exit(1);
    }

    printf("[port] Tick timer started: %u Hz (%u ms/tick)\n",
           g_tick_rate_hz, 1000 / g_tick_rate_hz);
}

/* -------------------------------------------------------------------
 *  Unix 临界区 (阻塞/解除 SIGALRM)
 *
 *  在修改就绪链表、延时链表等全局数据结构时,
 *  阻塞 SIGALRM 防止信号处理函数中断操作导致数据损坏。
 * ------------------------------------------------------------------- */

static sigset_t block_mask;
static int interrupts_disabled = 0;

void port_disable_interrupts(void)
{
    if (interrupts_disabled) return;
    interrupts_disabled = 1;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGALRM);
    sigprocmask(SIG_BLOCK, &block_mask, NULL);
}

void port_enable_interrupts(void)
{
    if (!interrupts_disabled) return;
    interrupts_disabled = 0;
    sigprocmask(SIG_UNBLOCK, &block_mask, NULL);
}

#endif /* _WIN32 */

/* ====================================================================
 *  跨平台共用 API
 * ==================================================================== */

/* 空闲等待: 模拟 WFI (Wait For Interrupt), 避免 100% CPU 占用 */
#ifdef _WIN32
void port_idle_wait(void) { Sleep(1); }       /* Windows: 1ms */
#else
void port_idle_wait(void) { usleep(1000); }   /* Unix: 1000μs = 1ms */
#endif

int  port_tick_pending(void) { return tick_pending; }
void port_clear_tick(void)   { tick_pending = 0; }

/* ====================================================================
 *  Tick 间隔 API (毫秒级, 直设硬件定时器)
 * ==================================================================== */

uint32_t kernel_get_tick_rate(void)
{
    return g_tick_rate_hz;
}

void kernel_set_tick_rate(uint32_t hz)
{
    if (hz < 1 || hz > 1000) {
        return;  /* 范围保护: 1Hz ~ 1000Hz */
    }

    port_disable_interrupts();

    g_tick_rate_hz = hz;

#ifdef _WIN32
    /* Windows: tick 线程每循环读取 g_tick_rate_hz，无需额外操作 */
    (void)0;
#else
    /* Unix: 若调度器已启动，重配置 setitimer */
    if (scheduler_running) {
        struct itimerval timer;
        uint32_t interval_us = 1000000 / hz;

        timer.it_value.tv_sec     = interval_us / 1000000;
        timer.it_value.tv_usec    = interval_us % 1000000;
        timer.it_interval.tv_sec  = interval_us / 1000000;
        timer.it_interval.tv_usec = interval_us % 1000000;

        setitimer(ITIMER_REAL, &timer, NULL);

        /* 清除旧频率下残留的 tick 标志 */
        port_clear_tick();
    }
#endif

    port_enable_interrupts();
}

/* 毫秒级 API — 直设硬件定时器间隔 */
void kernel_set_tick_interval_ms(uint32_t ms)
{
    if (ms < 1 || ms > 1000) return;  /* 1ms ~ 1000ms */
    kernel_set_tick_rate(1000 / ms);
}

uint32_t kernel_get_tick_interval_ms(void)
{
    return 1000 / g_tick_rate_hz;
}
