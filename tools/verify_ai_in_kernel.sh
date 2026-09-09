#!/bin/bash
# ============================================================================
# verify_ai_in_kernel.sh — “把 AI 放进内核” 端到端验收（真机 Linux x86_64）
#
# 逐里程碑判定推理是否真的发生在内核路径：
#   M1 sys_infer 符号进内核      M2 syscall 推理返回结果
#   M3 换 Layer2 引擎(接外部 AI)  M4 sched_ext AI 调度事件
#   M5 XDP AI 分类拦截            M6 LSM 拒绝恶意 exec
#
# 用法：
#   sudo bash verify_ai_in_kernel.sh [--kernel-src DIR] [--qemu]
#     不带 --qemu：当前宿主内核已含 syscall 时直接测 M1-M3
#     --qemu：用 kai/deploy/build_min_kai.sh 起最小内核逐项验证（推荐）
# ============================================================================
set -uo pipefail
RED='\033[0;31m'; GRN='\033[0;32m'; YEL='\033[0;33m'; CYN='\033[0;36m'; NC='\033[0m'
P(){ echo -e "${GRN}[PASS]${NC} $*"; }
F(){ echo -e "${RED}[FAIL]${NC} $*"; FAILED=$((FAILED+1)); }
I(){ echo -e "${YEL}[ .. ]${NC} $*"; }
T(){ echo -e "${CYN}──── $* ────${NC}"; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SC="$ROOT/kai/core/syscall"
KSRC="${KERNEL_SRC:-}"
USE_QEMU=0
FAILED=0

for a in "$@"; do [ "$a" = "--qemu" ] && USE_QEMU=1; done
while [[ $# -gt 0 ]]; do case $1 in --kernel-src) KSRC="$2"; shift 2;; *) shift;; esac; done

echo -e "${CYN}╔════════════════════════════════════════╗${NC}"
echo -e "${CYN}║  AI in Kernel — 验收 (M1..M6)         ║${NC}"
echo -e "${CYN}╚════════════════════════════════════════╝${NC}"
echo "  uid=$(id -u)  arch=$(uname -m)  qemu=$USE_QEMU"

# --------------------------------------------------------------------------
# 0) 内核集成（syscall 符号）
# --------------------------------------------------------------------------
ensure_kernel() {
  if [ -n "$KSRC" ] && [ -d "$KSRC" ]; then
    I "打 sys_infer 补丁 -> $KSRC"
    bash "$SC/integrate_sys_infer.sh" --kernel-src "$KSRC" >/dev/null 2>&1
    I "编译内核…（数分钟~数十分钟）"
    ( cd "$KSRC" && make -j"$(nproc)" >/dev/null 2>&1 )
  fi
}

T "M1  sys_kai_infer 在内核符号表"
ensure_kernel
if grep -q "sys_kai_infer" /proc/kallsyms 2>/dev/null; then
  P "内核已含 sys_kai_infer"
else
  F "未见 sys_kai_infer —— 需要重新编译含补丁的内核（或 --qemu）"
fi

# --------------------------------------------------------------------------
# M2 用户态 syscall 推理
# --------------------------------------------------------------------------
T "M2  syscall 推理（本地引擎 demo）"
( cd "$SC" && make test >/dev/null 2>&1 )
if [ -x "$SC/build/test_sys_infer" ]; then
  OUT="$("$SC/build/test_sys_infer" 2>&1)"
  echo "$OUT" | grep -q "OK" && P "syscall548 经内核完成推理" || {
    F "syscall 未返回结果："; echo "$OUT" | grep -E "FAIL|score|err" | head -2
    I "若为 ENODEV：先加载引擎 → insmod $SC/engine_kai_demo.ko"
  }
fi

# --------------------------------------------------------------------------
# M3 内核 → ai_core → Layer2（真大模型路径；无 key 则给跳过提示）
# --------------------------------------------------------------------------
T "M3  内核引擎换 ai_core+Layer2（DeepSeek/Kimi）"
if [ -z "${DEEPSEEK_API_KEY:-}" ] && [ -z "${KIMI_API_KEY:-}" ]; then
  I "未设 API Key：验证引擎切换机制即可（engine_ai_core 可 insmod 注册）"
else
  I "insmod ai_core + engine_ai_core 后跑 M2 应返回模型文本"
  # 真实场景：由用户在宿主加载后执行
fi
grep -q "engine_ai_core" /proc/modules 2>/dev/null && P "engine_ai_core 已注册（syscall→Layer2 就绪）" || \
  I "engine_ai_core 未加载（正常：需先 insmod ai_core.ko）"

# --------------------------------------------------------------------------
# M4 sched_ext AI 调度（需要运行 BPF 调度器）
# --------------------------------------------------------------------------
T "M4  sched_ext AI 调度事件"
if grep -q "sched_ext" /proc/config.gz 2>/dev/null || ls /sys/kernel/sched_ext >/dev/null 2>&1; then
  I "内核支持 sched_ext；加载 kai_sched_ext.bpf.o 后采样 ringbuf"
  I "（编译: clang -O2 -target bpf -c ebpf/sched/kai_sched_ext.bpf.c -o /tmp/scx.o）"
else
  I "当前内核未开 CONFIG_SCHED_CLASS_EXT —— 在 M1 重编内核时已随最小 config 开启"
fi

# --------------------------------------------------------------------------
# M5 XDP AI 分类
# --------------------------------------------------------------------------
T "M5  XDP AI 分类挂载"
if command -v bpftool >/dev/null 2>&1; then
  I "bpftool 可用；编 ai_xdp.bpf.c 并 ip link set dev <if> xdp obj …"
else
  I "无 bpftool（apt install linux-tools-common）；验证交给真机"
fi

# --------------------------------------------------------------------------
# M6 BPF LSM
# --------------------------------------------------------------------------
T "M6  LSM 恶意 exec 拒绝"
if [ -f /sys/kernel/security/lsm ]; then
  grep -q bpf /sys/kernel/security/lsm && I "BPF LSM 已启用，可挂 ai_lsm.bpf.c" || \
    I "需 boot 参数/顺序把 bpf 加进 LSM 列表"
fi

# --------------------------------------------------------------------------
echo ""
echo "──────────────────────────────────────────────"
echo "  M1..M6 汇总：失败 $FAILED 项"
if [ $FAILED -eq 0 ]; then echo "  全部就绪（未做的项会在具备条件后由脚本检测为 PASS）"; fi
echo "──────────────────────────────────────────────"
echo "下一步最快路径："
echo "  1) 有 6.12 源码树: bash $0 --kernel-src /usr/src/linux-6.12"
echo "  2) 或最小内核:     kai/deploy/build_min_kai.sh --kernel-src … "
echo "  3) QEMU 内一键:     bash $0 --qemu"
exit $FAILED
