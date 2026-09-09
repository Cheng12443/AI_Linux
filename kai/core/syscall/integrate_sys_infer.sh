#!/bin/bash
# ============================================================================
# integrate_sys_infer.sh — 把 sys_infer() 真正集成进 Linux 内核
#
# 对一个 linux-6.x 源码树执行：
#   1. 复制 kai_syscall_core.c → kernel/kai/
#   2. syscall 表注册 #548 (x86_64)；arm64 走 generic unistd.h
#   3. kernel/Kconfig + kernel/Makefile 挂载 CONFIG_KAI_SYSCALL
#   4. 完成清单检查
#
# 用法：
#   ./integrate_sys_infer.sh --kernel-src /path/to/linux-6.12
#   ./integrate_sys_infer.sh --kernel-src ... --dry-run   # 只打印将做什么
#
# 之后：
#   cd <内核源码>
#   make menuconfig   # 确保 KAI syscall 为 y（默认 y）
#   make -j$(nproc)
# ============================================================================

set -euo pipefail
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[0;33m'; NC='\033[0m'
log(){ echo -e "${GREEN}[kai-sys]${NC} $*"; }
warn(){ echo -e "${YELLOW}[kai-sys]${NC} $*"; }
die(){ echo -e "${RED}[kai-sys]${NC} $*" >&2; exit 1; }

SRC="${KERNEL_SRC:-}"
DRY=0
HERE="$(cd "$(dirname "$0")" && pwd)"
CORE="$HERE/kai_syscall_core.c"
NR=548
NAME="kai_infer"

while [[ $# -gt 0 ]]; do
  case $1 in
    --kernel-src) SRC="$2"; shift 2 ;;
    --dry-run)    DRY=1; shift ;;
    *) shift ;;
  esac
done

[ -n "$SRC" ] || die "请指定 --kernel-src /path/to/linux-6.x"
[ -f "$SRC/Makefile" ] && [ -d "$SRC/kernel" ] || die "不是有效内核源码树: $SRC"
[ -f "$CORE" ] || die "缺少 kai_syscall_core.c"

run(){ if [ $DRY -eq 1 ]; then echo "  [dry-run] $*"; else eval "$*"; fi }

# ============================================================================
log "1) 复制 syscall 实现 → kernel/kai/"
run "mkdir -p '$SRC/kernel/kai'"
run "cp '$CORE' '$SRC/kernel/kai/kai_syscall_core.c'"
run "cp '$HERE/engine_kai_demo.c' '$SRC/kernel/kai/engine_kai_demo.c' 2>/dev/null || true"

# ============================================================================
# 2) syscall 表注册
# ============================================================================
ARCH_MARK=""
TBL=""
if [ -f "$SRC/arch/x86/entry/syscalls/syscall_64.tbl" ]; then
  TBL="$SRC/arch/x86/entry/syscalls/syscall_64.tbl"
  ARCH_MARK="x86_64"
elif [ -f "$SRC/include/uapi/asm-generic/unistd.h" ]; then
  TBL="$SRC/include/uapi/asm-generic/unistd.h"
  ARCH_MARK="generic/arm64"
else
  die "未识别的 syscall 表（支持 x86_64 / asm-generic）"
fi
log "2) syscall 表: $ARCH_MARK → $TBL"

if grep -qE "kai_infer|$NR\s+common\s+$NAME" "$TBL" 2>/dev/null; then
  log "   syscall #$NR 已注册，跳过"
else
  case "$ARCH_MARK" in
    x86_64)
      # 在 x32 ABI 段之前插入
      ANCHOR=$(grep -n "x32" "$TBL" | head -1 | cut -d: -f1)
      if [ -z "$ANCHOR" ]; then
        run "echo '$NR	common	$NAME	sys_$NAME' >> '$TBL'"
      else
        run "sed -i '${ANCHOR}i\\
$NR	common	$NAME	sys_$NAME' '$TBL'"
      fi
      log "   已插入: $NR common $NAME sys_$NAME"
      ;;
    generic)
      warn "generic/arm64 使用宏表，需手工在 #ifndef __NR_$NAME 前加："
      warn "  #define __NR_$NAME $NR"
      warn "  __SYSCALL(__NR_$NAME, sys_$NAME)"
      die "arm64 请手动添加（见 README）"
      ;;
  esac
fi

# ============================================================================
# 3) Kconfig + Makefile
# ============================================================================
log "3) 挂载 CONFIG_KAI_SYSCALL"

if grep -q "config KAI_SYSCALL" "$SRC/kernel/Kconfig"; then
  log "   Kconfig 已含 KAI_SYSCALL"
else
  cat >> "$SRC/kernel/Kconfig" <<'KAICONF'

menu "KAI Linux - AI syscall"
config KAI_SYSCALL
	bool "sys_infer(): AI inference system call (#548)"
	depends on KALLSYMS
	default y
	help
	  Enables syscall 548 (sys_kai_infer). Any privileged process may
	  issue an AI inference to a kernel-registered engine. Engines are
	  hot-plugged via kai_engine_register() (see engine_kai_demo.ko).
	  Say N here to keep the table entry inert.
endmenu
KAICONF
  log "   已写入 kernel/Kconfig（追加 KAI menu 块）"
fi

if grep -q "KAI_SYSCALL" "$SRC/kernel/Makefile"; then
  log "   Makefile 已含 kai 目标"
else
  echo 'obj-$(CONFIG_KAI_SYSCALL) += kai/kai_syscall_core.o' >> "$SRC/kernel/Makefile"
  log "   已写入 kernel/Makefile"
fi

# ============================================================================
# 4) 完成清单
# ============================================================================
echo ""
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  sys_infer() 集成完成清单${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
[ $DRY -eq 1 ] && echo "（dry-run：以上为将执行的操作）"
echo ""
echo " [集成产物]"
[ -f "$SRC/kernel/kai/kai_syscall_core.c" ] && echo "  ✓ kernel/kai/kai_syscall_core.c" || echo "  · kernel/kai/…"
grep -qE "^$NR.*kai_infer" "$SRC/arch/x86/entry/syscalls/syscall_64.tbl" 2>/dev/null && \
  echo "  ✓ syscall_64.tbl: #$NR sys_$NAME" || echo "  · syscall 表"
grep -q "config KAI_SYSCALL" "$SRC/kernel/Kconfig" && echo "  ✓ kernel/Kconfig: CONFIG_KAI_SYSCALL" || echo "  · Kconfig"
grep -q "KAI_SYSCALL" "$SRC/kernel/Makefile" && echo "  ✓ kernel/Makefile" || echo "  · Makefile"

echo ""
echo " [编译并验证]"
echo "  cd '$SRC'"
echo "  make olddefconfig        # 或 make menuconfig 检查 KAI_SYSCALL=y"
echo "  make -j\$(nproc)"
echo ""
echo " [加载演示推理引擎]"
echo "  make -C '$SRC' M='$SRC/kernel/kai' modules"
echo "  insmod '$SRC/kernel/kai/engine_kai_demo.ko'"
echo ""
echo " [用户态冒烟]"
echo "  gcc '$HERE/test_sys_infer.c' -o /tmp/tsi && /tmp/tsi"
echo ""
echo " [在线检查]"
echo "  grep kai_infer /proc/kallsyms   # 应见 sys_kai_infer"
echo ""
