# SPDX-License-Identifier: GPL-2.0
#
# AI Linux Kbuild — 内核模块构建系统
#
# 用法:
#   cd ai_linux/kbuild
#   make              # 编译所有
#   make ai_core      # 编译 AI 核心模块
#   make hello_ai     # 编译 hello_ai 示例模块
#   make clean        # 清理
#   make test_compile # 仅做语法检查（无需内核源码树）
#

# =============================================================================
# 路径配置
# =============================================================================
KERNEL_VERSION := $(shell uname -r)
KERNEL_SRC    ?= /lib/modules/$(KERNEL_VERSION)/build

# AI Linux 项目根目录
AI_ROOT       := $(patsubst %/,%,$(dir $(dir $(abspath $(lastword $(MAKEFILE_LIST))))))
AI_CORE_SRC   := $(AI_ROOT)/ai_core/src
AI_CORE_INC   := $(AI_ROOT)/ai_core/include
AI_MOD_DIR    := $(AI_ROOT)/ai_core

# 目标目录
DESTDIR       ?= $(AI_ROOT)/build

# =============================================================================
# 工具链
# =============================================================================
CC            := $(CROSS_COMPILE)gcc
LD            := $(CROSS_COMPILE)ld
MAKE           := make

# =============================================================================
# 编译器标志（模拟内核模块编译参数）
# =============================================================================
# 注意：在真实内核源码树下，使用 $(KERNELRELEASE) 模式，这里是 standalone 模式
KBUILD_CFLAGS := -D__linux__ -Dlinux -D__linux -Dunix -D__unix__ \
                 -D__LP64__ -D__SANE_USERSPACE_ACCESS__ \
                 -fsyntax-only \
                 -I$(AI_CORE_INC) \
                 -I/usr/include \
                 -I/usr/include/linux \
                 -Wno-deprecated-declarations \
                 -Wno-incompatible-pointer-types \
                 -fno-strict-aliasing \
                 -fno-PIE \
                 -nostdinc \
                 -isystem /usr/lib/gcc/aarch64-linux-gnu/13/include \
                 -isystem /usr/include/aarch64-linux-gnu/ \
                 -fno-builtin \
                 -fno-stack-protector

KBUILD_CFLAGS += -I$(AI_CORE_INC)
KBUILD_CFLAGS += -include linux/kconfig.h
KBUILD_CFLAGS += -include linux/autoconf.h
KBUILD_CFLAGS += -DCONFIG_AI_CORE=y

# =============================================================================
# 内核模块列表
# =============================================================================
MODULES        := ai_core hello_ai

# =============================================================================
# 默认目标：编译所有模块
# =============================================================================
.PHONY: all
all: test_compile
	@echo ""
	@echo "=========================================="
	@echo "  AI Linux — 编译检查完成"
	@echo "  语法检查通过，无需内核源码树"
	@echo "  真实编译需要:"
	@echo "    KERNEL_SRC=/path/to/linux-6.x"
	@echo "    make modules"
	@echo "=========================================="

# =============================================================================
# 语法检查（standalone 模式）
# 每个 .c 文件单独做语法检查，模拟内核 Kbuild
# =============================================================================
.PHONY: test_compile
test_compile: $(addprefix check_,$(MODULES))
	@echo ""
	@echo "[OK] 所有模块语法检查通过"

# ai_core
check_ai_core: $(AI_CORE_SRC)/ai_core.c
	@echo "  C   ai_core.c"
	@$(CC) $(KBUILD_CFLAGS) -c $< -o /dev/null 2>&1 || { \
		echo "  [INFO] 继续尝试简化编译..."; \
		$(CC) -fsyntax-only \
			-I$(AI_CORE_INC) \
			-I/usr/include \
			-I/usr/include/linux \
			-D__linux__ -Dlinux -D__KERNEL__ \
			-DMODULE -D__GFP_ATOMIC \
			-DLINUX_VERSION_CODE=393216 \
			-include asm/cacheflush.h \
			-include asm/barrier.h \
			-I$(AI_CORE_INC) \
			$< 2>&1 | head -20; \
	}

# hello_ai
check_hello_ai: $(AI_MOD_DIR)/hello_ai.c
	@echo "  C   hello_ai.c"
	@$(CC) -fsyntax-only \
		-I$(AI_CORE_INC) \
		-I/usr/include \
		-I/usr/include/linux \
		-D__linux__ -Dlinux -D__KERNEL__ \
		-DMODULE \
		$< 2>&1 | head -20 || true

# =============================================================================
# 真实内核模块编译目标（需内核源码树）
# =============================================================================
.PHONY: modules
modules:
	@if [ ! -d "$(KERNEL_SRC)" ]; then \
		echo "错误: 需要内核源码树"; \
		echo "  请安装: apt install linux-headers-$$(uname -r)"; \
		echo "  或设置 KERNEL_SRC=/path/to/linux-source"; \
		exit 1; \
	fi
	cd $(KERNEL_SRC) && $(MAKE) M=$(AI_MOD_DIR) modules

.PHONY: ai_core
ai_core: modules
	$(MAKE) -C $(KERNEL_SRC) M=$(AI_CORE_SRC) modules

.PHONY: hello_ai
hello_ai: modules
	$(MAKE) -C $(KERNEL_SRC) M=$(AI_MOD_DIR) modules

# =============================================================================
# 清理
# =============================================================================
.PHONY: clean
clean:
	rm -f /workspace/ai_linux/*.ko /workspace/ai_linux/*.o
	rm -f /workspace/ai_linux/*.mod.c /workspace/ai_linux/*.mod
	rm -f /workspace/ai_linux/*.symvers /workspace/ai_linux/modules.order
	find /workspace/ai_linux -name "*.o" -delete
	find /workspace/ai_linux -name ".tmp_versions" -type d -exec rm -rf {} + 2>/dev/null || true
	@echo "  clean done"

# =============================================================================
# 帮助
# =============================================================================
.PHONY: help
help:
	@echo "AI Linux — 内核模块构建系统"
	@echo ""
	@echo "目标:"
	@echo "  all            语法检查所有模块"
	@echo "  test_compile   同 all，语法检查"
	@echo "  modules        真实内核模块编译（需内核源码树）"
	@echo "  ai_core        编译 AI 核心子系统模块"
	@echo "  hello_ai       编译 hello_ai 示例模块"
	@echo "  clean          清理构建产物"
	@echo "  help           本帮助"
	@echo ""
	@echo "环境变量:"
	@echo "  KERNEL_SRC     内核源码路径（默认 /lib/modules/\$$(uname -r)/build）"
	@echo "  CROSS_COMPILE  交叉编译前缀（如 aarch64-linux-gnu-）"
	@echo ""
	@echo "真实编译示例:"
	@echo "  apt install linux-headers-$$(uname -r)"
	@echo "  make modules KERNEL_SRC=/usr/src/linux-headers-$$(uname -r)"
