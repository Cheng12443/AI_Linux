# SPDX-License-Identifier: GPL-2.0
#
# KAI Linux — Makefile
#
# 编译：
#   make              # 全部编译
#   make kai_infer    # 推理引擎
#   make kai_cache    # 缓存系统
#   make kai_syscall  # 系统调用
#   make kai_quant    # 量化支持

KERNEL_VERSION := $(shell uname -r)
KERNEL_SRC    ?= /lib/modules/$(KERNEL_VERSION)/build
MODULES        := kai_infer kai_cache kai_syscall kai_resilience kai_metrics

# 目标目录
KAI_ROOT := $(shell pwd)
BUILD    := $(KAI_ROOT)/build

# 编译标志
ccflags-y := -I$(KAI_ROOT)/core -I$(KAI_ROOT)/cache
ccflags-y += -I$(KAI_ROOT)/../ai_core/include

# 对象文件
kai_infer-objs := core/kai_infer.o cache/kai_cache.o
kai_cache-objs := cache/kai_cache.o
kai_syscall-objs := core/kai_syscall.o

obj-m += kai_infer.o kai_cache.o kai_syscall.o kai_resilience.o kai_metrics.o

.PHONY: all clean help

all: $(MODULES)
	@echo ""
	@echo "  KAI 模块编译完成"
	@echo "  insmod $(BUILD)/kai_cache.ko"
	@echo "  insmod $(BUILD)/kai_infer.ko"
	@echo "  insmod $(BUILD)/kai_syscall.ko"
	@echo ""

$(BUILD):
	mkdir -p $(BUILD)

# 内核模块编译
modules: $(BUILD)
	@if [ ! -d "$(KERNEL_SRC)" ]; then \
		echo "错误: 需要内核源码树"; \
		echo "  apt install linux-headers-$$(uname -r)"; \
		exit 1; \
	fi
	$(MAKE) -C $(KERNEL_SRC) M=$(KAI_ROOT) modules
	@mv *.ko *.o *.mod.c .tmp_versions Module.symvers modules.order $(BUILD)/ 2>/dev/null || true
	@echo "  编译完成: $(BUILD)/"

# 独立编译（无内核源码树，仅语法检查）
check:
	@echo "  语法检查..."
	@gcc -fsyntax-only \
		-I$(KAI_ROOT)/core -I$(KAI_ROOT)/cache \
		-I$(KAI_ROOT)/../ai_core/include \
		-I/usr/include -I/usr/include/linux \
		-D__linux__ -Dlinux -D__KERNEL__ \
		core/kai_infer.c 2>&1 | head -10 || true
	@gcc -fsyntax-only \
		-I$(KAI_ROOT)/core -I$(KAI_ROOT)/cache \
		-I$(KAI_ROOT)/../ai_core/include \
		-D__linux__ -Dlinux -D__KERNEL__ \
		cache/kai_cache.c 2>&1 | head -10 || true
	@echo "  语法检查完成"

clean:
	rm -rf $(BUILD)/*.ko $(BUILD)/*.o
	rm -rf $(BUILD)/.tmp_versions
	@echo "  clean done"

help:
	@echo "KAI Linux Makefile"
	@echo ""
	@echo "  make            编译所有模块（需内核源码树）"
	@echo "  make modules    同上"
	@echo "  make check      语法检查（无需内核源码树）"
	@echo "  make clean      清理"
	@echo ""
