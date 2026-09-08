# SPDX-License-Identifier: GPL-2.0
#
# AI Linux — 根目录 Makefile
#

.PHONY: all help clean test-syntax build-ebpf build-modules build-userland

KBUILD_DIR := kbuild
EBPF_DIR   := ebpf
SCRIPTS    := scripts

all: help

help:
	@echo ""
	@echo "  AI Linux — 内核级 AI 推理系统"
	@echo ""
	@echo "  构建目标："
	@echo "    make syntax     语法检查（无需内核源码树）"
	@echo "    make ebpf        编译 eBPF 程序"
	@echo "    make modules     编译内核模块（需内核源码树）"
	@echo "    make userland    编译用户态程序"
	@echo "    make test        运行测试"
	@echo "    make all         全部构建"
	@echo ""
	@echo "  内核模块加载（需 root）："
	@echo "    insmod build/ai_core.ko"
	@echo "    insmod build/hello_ai.ko"
	@echo "    cat /proc/hello_ai/stats"
	@echo ""
	@echo "  eBPF 加载："
	@echo "    bpftool sched replace ./ebpf/sched/ai_sched.bpf.o ai_sched"
	@echo "    ip link set eth0 xdp obj ./ebpf/io/ai_xdp.bpf.o sec xdp"
	@echo ""
	@echo "  详细文档：docs/ARCHITECTURE.md"
	@echo ""

syntax:
	@echo "=== 语法检查 ==="
	@make -C $(KBUILD_DIR) test_compile

build-ebpf:
	@echo "=== 编译 eBPF 程序 ==="
	@bash $(SCRIPTS)/build_all.sh --ebpf

build-userland:
	@echo "=== 编译用户态程序 ==="
	@bash $(SCRIPTS)/build_all.sh --userland

build-modules:
	@echo "=== 编译内核模块 ==="
	@bash $(SCRIPTS)/build_all.sh --kernel

test:
	@echo "=== 运行测试 ==="
	@python3 $(SCRIPTS)/../tests/test_ai_core.py

clean:
	@echo "=== 清理 ==="
	@find . -name "*.o" -delete
	@find . -name "*.ko" -delete
	@find . -name ".tmp_versions" -type d -exec rm -rf {} + 2>/dev/null || true
	@rm -rf build/
	@echo "  clean done"
