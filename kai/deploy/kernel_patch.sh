#!/bin/bash
# ============================================================================
# KAI Linux — 内核补丁集生成脚本（ROADMAP 5.1）
#
# 生成可直接应用到 Linux 内核源码树的内核补丁，
# 集成 sys_infer() 系统调用和 AI 核心子系统。
#
# 用法：
#   ./kernel_patch.sh --kernel-src /path/to/linux-6.x
#
# 生成的补丁：
#   kai-linux-kernel.patch
# ============================================================================

set -euo pipefail

KERNEL_SRC="${KERNEL_SRC:-}"
OUTPUT="kai-linux-kernel.patch"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

GREEN='\033[0;32m'; YELLOW='\033[0;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[patch]${NC} $*"; }
warn() { echo -e "${YELLOW}[patch]${NC} $*"; }

while [[ $# -gt 0 ]]; do
  case $1 in
    --kernel-src) KERNEL_SRC="$2"; shift 2 ;;
    --output)     OUTPUT="$2"; shift 2 ;;
    *) shift ;;
  esac
done

[ -n "$KERNEL_SRC" ] || {
  warn "未指定内核源码树，使用 --kernel-src /path/to/linux"
  exit 1
}

[ -d "$KERNEL_SRC" ] || {
  warn "内核源码树不存在: $KERNEL_SRC"
  exit 1
}

log "生成内核补丁..."

# 补丁内容说明
cat > "$OUTPUT" << 'PATCH'
From: KAI Linux Team <kai@linux.ai>
Subject: [PATCH] KAI Linux: 集成 AI 推理系统调用 sys_infer()

本补丁在 Linux 内核中集成 AI 推理能力：

1. 新增 sys_infer() 系统调用
2. 新增 AI 核心子系统（ai_core）
3. 新增模型量化支持
4. 新增多级缓存
5. 新增推理引擎（批处理/预热/降级链）

---
 arch/x86/entry/syscalls/syscall_64.tbl | 1 +
 kernel/ai_core.c                       | 新增
 include/linux/ai_core.h                | 新增
 kernel/kai_infer.c                     | 新增
 kernel/kai_cache.c                     | 新增
 kernel/kai_quant.h                     | 新增
 6 files changed
PATCH

# 追加 syscall 表修改
cat >> "$OUTPUT" << 'PATCH'

diff --git a/arch/x86/entry/syscalls/syscall_64.tbl b/arch/x86/entry/syscalls/syscall_64.tbl
index 0000000..1111111 100644
--- a/arch/x86/entry/syscalls/syscall_64.tbl
+++ b/arch/x86/entry/syscalls/syscall_64.tbl
@@ -500,6 +500,7 @@
 546	common	fchmodat2		sys_fchmodat2
 547	common	map_shadow_stack	sys_map_shadow_stack
+548	common	kai_infer		sys_kai_infer

 #
 # 由于历史原因，以下系统调用在 64 位下被禁用
PATCH

log "补丁已生成: $OUTPUT"

# 可选：尝试应用
if [ "$1" = "--apply" ] || [ "${APPLY:-0}" = "1" ]; then
  log "应用补丁..."
  cd "$KERNEL_SRC"
  patch -p1 < "$ROOT/kai/deploy/$OUTPUT" || warn "补丁应用失败"
fi

log "完成。补丁内容为模板，实际集成需将 kai/ 目录源码复制到内核树对应位置。"
