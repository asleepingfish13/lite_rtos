# Lite RTOS - Makefile (跨平台)
#   macOS / Linux: ucontext + SIGALRM
#   Windows MSYS2:  Fibers + 后台线程
#
# 使用方法:
#   make          - 编译
#   make run      - 编译并运行
#   make clean    - 清除构建
#   make info     - 显示平台信息

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
LDFLAGS  =

BUILD_DIR = build
TARGET    = $(BUILD_DIR)/lite_rtos_demo$(EXE_EXT)

# 源文件
SRCS  = list.c
SRCS += kernel.c
SRCS += task.c
SRCS += port.c
SRCS += demo.c

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
	@echo "Sources:     $(SRCS)"
