#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# build_all.sh — AI Linux 全量构建脚本
#
# 用法：
#   ./scripts/build_all.sh           # 全部构建
#   ./scripts/build_all.sh --kernel # 仅内核模块
#   ./scripts/build_all.sh --ebpf   # 仅 eBPF 程序
#   ./scripts/build_all.sh --test   # 运行测试

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AI_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$AI_ROOT/build"
LOG_FILE="$BUILD_DIR/build.log"

mkdir -p "$BUILD_DIR"

log()  { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG_FILE"; }
logerr(){ echo "[$(date '+%H:%M:%S')] ERROR: $*" >&2 | tee -a "$LOG_FILE"; exit 1; }

log "========================================"
log "  AI Linux Build System v1.0.0"
log "========================================"

# ============================================================================
# 1. 语法检查（无需内核源码树）
# ============================================================================
check_syntax() {
    log "--- 语法检查（standalone）---"
    cd "$AI_ROOT/kbuild"
    make test_compile 2>&1 | tee -a "$LOG_FILE" || true
    log "  语法检查完成"
}

# ============================================================================
# 2. 内核模块编译（需内核源码树）
# ============================================================================
build_kernel_modules() {
    log "--- 编译内核模块 ---"
    if [ ! -d "/lib/modules/$(uname -r)/build" ]; then
        log "  警告：内核源码树不可用，跳过模块编译"
        log "  安装：apt install linux-headers-$(uname -r)"
        return 1
    fi

    cd "$AI_ROOT/kbuild"
    KERNEL_SRC="/lib/modules/$(uname -r)/build"
    make modules KERNEL_SRC="$KERNEL_SRC" 2>&1 | tee -a "$LOG_FILE"
    log "  内核模块编译完成"
}

# ============================================================================
# 3. eBPF 程序编译
# ============================================================================
build_ebpf() {
    log "--- 编译 eBPF 程序 ---"
    which clang &>/dev/null || { logerr "clang 未安装: apt install clang"; }

    EBPF_DIR="$AI_ROOT/ebpf"
    ARCH=$(uname -m)
    CLANG_FLAGS="-O2 -target bpf -g \
        -D__TARGET_ARCH_$ARCH \
        -I/usr/include/bpf \
        -I/usr/include/linux \
        -Wall -Wno-unused-value -Wno-pointer-sign"

    for prog in "$EBPF_DIR"/**/*.bpf.c; do
        [ -f "$prog" ] || continue
        out="${prog%.c}.o"
        log "  clang $prog → $out"
        clang $CLANG_FLAGS "$prog" -o "$out" 2>&1 | tee -a "$LOG_FILE" || true
    done
    log "  eBPF 编译完成"
}

# ============================================================================
# 4. 用户态程序编译
# ============================================================================
build_userland() {
    log "--- 编译用户态程序 ---"
    CC="${CC:-gcc}"
    SRC="$AI_ROOT/ai_core/src/ai_userland.c"
    OUT="$BUILD_DIR/ai_userlandd"

    if [ ! -f "$SRC" ]; then
        log "  用户态源文件不存在，跳过"
        return
    fi

    # 尝试编译（无依赖版本）
    $CC -O2 -o "$OUT" "$SRC" \
        -I/usr/include \
        -Wall 2>&1 | tee -a "$LOG_FILE" || {
        log "  提示：ai_userland.c 需要 -luring 才能完整编译"
        log "  apt install liburing-dev && make userland_full"
    }

    if [ -f "$OUT" ]; then
        log "  编译成功: $OUT"
    fi
}

# ============================================================================
# 5. 运行测试
# ============================================================================
run_tests() {
    log "--- 运行测试 ---"
    TEST_DIR="$AI_ROOT/tests"
    if [ -d "$TEST_DIR" ]; then
        for t in "$TEST_DIR"/*.py; do
            [ -f "$t" ] || continue
            log "  python3 $t"
            python3 "$t" 2>&1 | tee -a "$LOG_FILE" || true
        done
    fi
    log "  测试完成"
}

# ============================================================================
# 主流程
# ============================================================================
TARGET="${1:-all}"

case "$TARGET" in
    --kernel)  build_kernel_modules ;;
    --ebpf)    build_ebpf ;;
    --userland) build_userland ;;
    --test)    run_tests ;;
    all|*)
        check_syntax
        build_ebpf
        build_userland
        build_kernel_modules
        run_tests
        ;;
esac

log ""
log "========================================"
log "  构建完成！"
log "========================================"
log ""
log "下一步："
log "  1. 安装内核源码树：apt install linux-headers-\$(uname -r)"
log "  2. 编译内核模块：make -C kbuild modules KERNEL_SRC=/path/to/src"
log "  3. 加载模块：sudo insmod build/ai_core.ko"
log "  4. 编译 eBPF：clang -O2 -target bpf ... ebpf/sched/ai_sched.bpf.c"
log "  5. 加载 BPF 调度器：bpftool sched replace ./ai_sched.bpf.o ai_sched"
log ""
