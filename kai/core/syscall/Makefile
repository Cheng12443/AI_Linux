# SPDX-License-Identifier: GPL-2.0
# sys_infer() 用户态构建（test / libkai），以及引擎 .ko 模块
#
# 用法：
#   make test              # 编译用户态测试（test_sys_infer + 扩展 tests）
#   make libkai            # 编 libkai.a / 示例 demo_sys_infer
#   make engines           # 内核模块（需 KERNEL_SRC 或内核树内）
#
# 内核树内编译引擎（打补丁后）：
#   make -C $KERNEL_SRC M=$PWD modules
#
# 或者把 engines/*.c 放进内核 kernel/kai/ 后：
#   make -C $KERNEL_SRC M=$KERNEL_SRC/kernel/kai modules

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
BUILD   := build
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

.PHONY: all test libkai engines clean help

all: libkai test

$(BUILD):
	mkdir -p $(BUILD)

# ---- 用户态测试 ----
test: $(BUILD)/test_sys_infer $(BUILD)/testsuite
	@echo "  测试二进制就绪:"
	@echo "    ./build/test_sys_infer   # 冒烟"
	@echo "    ./build/testsuite        # 多场景回归（需内核已集成 syscall548）"

$(BUILD)/test_sys_infer: test_sys_infer.c libkai/kai_syscall.c | $(BUILD)
	$(CC) $(CFLAGS) -Ilibkai -o $@ test_sys_infer.c libkai/kai_syscall.c

$(BUILD)/testsuite: tests/testsuite.c libkai/kai_syscall.c | $(BUILD)
	$(CC) $(CFLAGS) -Ilibkai -lpthread -o $@ tests/testsuite.c libkai/kai_syscall.c

# ---- libkai 静态库 ----
libkai: $(BUILD)/libkai.a
$(BUILD)/libkai.a: libkai/kai_syscall.c libkai/kai_syscall.h | $(BUILD)
	$(CC) $(CFLAGS) -Ilibkai -c libkai/kai_syscall.c -o $(BUILD)/kai_syscall.o
	ar rcs $@ $(BUILD)/kai_syscall.o
	@echo "  -> $(BUILD)/libkai.a"
	$(CC) $(CFLAGS) -Ilibkai -o $(BUILD)/demo_sys_infer \
	    libkai/examples/demo.c $(BUILD)/libkai.a

# ---- 内核引擎模块（真机内核树内） ----
engines:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR)/engines modules 2>/dev/null || \
	  echo "提示：把 engines/ 放内核树 kernel/kai/ 后用 make -C 内核树 M=… 编译"

clean:
	rm -rf $(BUILD)

help:
	@echo "sys_infer() 用户态/引擎构建"
	@echo "  make test      编译冒烟 + 多场景回归测试"
	@echo "  make libkai    编译 libkai.a + demo"
	@echo "  make engines   内核引擎 .ko（需内核源码）"
