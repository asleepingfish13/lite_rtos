# Lite RTOS - Makefile (跨平台)
#   macOS / Linux: ucontext + SIGALRM
#   Windows MSYS2:  Fibers + 后台线程
#
# 使用方法:
#   make              - 编译 (默认 demo_yield.c)
#   make run          - 编译并运行
#   make DEMO=demo_list      - 切换不同 demo
#   make run DEMO=demo_list  - 编译并运行指定 demo
#   make list-demos   - 列出所有可用 demo
#   make clean        - 清除构建
#   make info         - 显示平台信息
#
# 可用 DEMO:
#   demo_list        - Phase 1: 侵入式链表
#   demo_task_create - Phase 2: 任务创建
#   demo_yield       - Phase 3-4: 合作式调度 (默认)
#   demo_delay       - Phase 5: 系统节拍与延时
#   demo_preempt      - Phase 6: 抢占式调度
#   demo_preempt_true - Phase 7: 真抢占 (ISR 驱动)
#   demo_compute       - Phase 8: 真实计算 + 抢占验证
#   demo_car          - 综合: 智能小车巡线 + 多传感器 + 拍照
#   demo_tick_rate    - Phase 9: 可变 tick 率 (环境监测 vs 小车巡线)
#   demo_sem          - Phase 11: 计数信号量 (Counting Semaphore) + 小车传感器场景

# -------------------------------------------------------------------
# 平台自动检测
# -------------------------------------------------------------------
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(UNAME_S),Darwin)
    # macOS
    PLATFORM := macOS
    EXE_EXT  :=
    CFLAGS   += -D_XOPEN_SOURCE=600 -Wno-deprecated-declarations

else ifeq ($(UNAME_S),Linux)
    # Linux
    PLATFORM := Linux
    EXE_EXT  :=
    CFLAGS   += -D_XOPEN_SOURCE=600

else ifneq ($(findstring MINGW,$(UNAME_S)),)
    # Windows + MSYS2 (MinGW-w64)
    PLATFORM := Windows-MSYS2
    EXE_EXT  := .exe
    # 使用 Windows Fibers, 不需要额外的编译标志

else ifneq ($(findstring MSYS,$(UNAME_S)),)
    # Windows + MSYS2 (MSYS 环境)
    PLATFORM := Windows-MSYS
    EXE_EXT  := .exe

else
    # 未知平台, 假设 Windows
    PLATFORM := Unknown
    EXE_EXT  := .exe
    $(warning Unknown platform: $(UNAME_S). Assuming Windows.)
endif

# -------------------------------------------------------------------
# 工具链和标志
# -------------------------------------------------------------------
CC       = gcc
CFLAGS  += -Wall -Wextra -Wshadow -g -O0
LDFLAGS  = -lm

BUILD_DIR = build
TARGET    = $(BUILD_DIR)/lite_rtos_demo$(EXE_EXT)

# 选择 Demo 文件
DEMO ?= demo_yield

# demo_list 只需要链表模块，不需要内核
ifeq ($(DEMO),demo_list)
    SRCS  = list.c
    SRCS += $(DEMO).c
else
    SRCS  = list.c
    SRCS += kernel.c
    SRCS += task.c
    SRCS += port.c
    SRCS += sem.c
    SRCS += $(DEMO).c
endif

OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)
DEPS = $(OBJS:%.o=%.d)

# -------------------------------------------------------------------
# 构建目标
# -------------------------------------------------------------------
.PHONY: all
all: $(BUILD_DIR) $(TARGET)
	@echo "✓ Build complete for $(PLATFORM): $(TARGET)"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

-include $(DEPS)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

.PHONY: run
run: all
	@echo "=== Lite RTOS Demo ($(PLATFORM)) ==="
	$(TARGET)

.PHONY: info
info:
	@echo "Platform:    $(PLATFORM) ($(UNAME_S))"
	@echo "Compiler:    $(shell $(CC) --version | head -1)"
	@echo "Target:      $(TARGET)"
	@echo "Demo:        $(DEMO)"
	@echo "Sources:     $(SRCS)"

.PHONY: list-demos
list-demos:
	@echo "============================================"
	@echo "  Lite RTOS - 可用 Demo 列表"
	@echo "============================================"
	@echo "  make DEMO=demo_list        - Phase 1: 侵入式链表"
	@echo "  make DEMO=demo_task_create - Phase 2: 任务创建"
	@echo "  make DEMO=demo_yield       - Phase 3-4: 合作式调度"
	@echo "  make DEMO=demo_delay       - Phase 5: 系统节拍与延时"
	@echo "  make DEMO=demo_preempt     - Phase 6: 抢占式调度"
	@echo "  make DEMO=demo_preempt_true- Phase 7: 真抢占 (ISR 驱动)"
	@echo ""
	@echo "  默认: make = make DEMO=demo_yield"
	@echo "  make DEMO=demo_compute      - Phase 8: 真实计算 + 抢占验证"
	@echo "  make DEMO=demo_car         - 综合: 小车巡线 + 多传感器 + 拍照"
	@echo "  make DEMO=demo_tick_rate   - Phase 9: 可变 tick 率 (环境监测 vs 小车巡线)"
	@echo "  make DEMO=demo_sem         - Phase 11: 计数信号量 + 小车传感器 (4 任务)"
	@echo ""
	@echo "  示例: make run DEMO=demo_car"
