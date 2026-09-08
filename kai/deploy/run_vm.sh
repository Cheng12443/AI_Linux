#!/bin/bash
# ============================================================================
# KAI Linux — 虚拟机启动脚本
#
# 启动 KAI Linux 磁盘镜像。
# 支持 QEMU / KVM。
#
# 用法：
#   ./run_vm.sh                        # 默认配置启动
#   ./run_vm.sh --image kai.qcow2      # 指定镜像
#   ./run_vm.sh --mem 8192 --smp 8     # 指定内存/CPU
#   ./run_vm.sh --kvm                  # 使用 KVM 加速
#   ./run_vm.sh --graphic              # 图形界面（默认无头）
# ============================================================================

set -euo pipefail

GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'

IMAGE="${IMAGE:-./kai-linux.qcow2}"
MEM="${MEM:-4096}"
SMP="${SMP:-4}"
USE_KVM=0
GRAPHIC=0

while [[ $# -gt 0 ]]; do
  case $1 in
    --image)   IMAGE="$2"; shift 2 ;;
    --mem)     MEM="$2"; shift 2 ;;
    --smp)     SMP="$2"; shift 2 ;;
    --kvm)     USE_KVM=1; shift ;;
    --graphic) GRAPHIC=1; shift ;;
    *) shift ;;
  esac
done

[ -f "$IMAGE" ] || { echo "错误: 镜像不存在 $IMAGE"; exit 1; }

# 检测架构
ARCH=$(uname -m)
case "$ARCH" in
  x86_64) QEMU_BIN="qemu-system-x86_64" ;;
  aarch64) QEMU_BIN="qemu-system-aarch64" ;;
  *) QEMU_BIN="qemu-system-x86_64" ;;
esac

command -v "$QEMU_BIN" >/dev/null 2>&1 || {
  echo "错误: 找不到 $QEMU_BIN"
  echo "  apt install qemu-system-x86"
  exit 1
}

# 构建 QEMU 参数
QEMU_ARGS=(
  -drive file="$IMAGE",format=qcow2
  -m "$MEM"
  -smp "$SMP"
  -net nic
  -net user,hostfwd=tcp::8080-:8080,hostfwd=tcp::9090-:9090,hostfwd=tcp::9998-:9998,hostfwd=tcp::2222-:22
  -machine accel=tcg
)

if [ "$USE_KVM" -eq 1 ]; then
  QEMU_ARGS=("${QEMU_ARGS[@]}" -enable-kvm)
fi

if [ "$GRAPHIC" -eq 0 ]; then
  QEMU_ARGS=("${QEMU_ARGS[@]}" -nographic)
fi

echo ""
echo -e "${CYAN}╔═══════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║   KAI Linux VM 启动器                    ║${NC}"
echo -e "${CYAN}╚═══════════════════════════════════════════╝${NC}"
echo ""
echo -e "  镜像: ${GREEN}$IMAGE${NC}"
echo -e "  内存: ${GREEN}${MEM}MB${NC}"
echo -e "  CPU:  ${GREEN}${SMP} 核${NC}"
echo -e "  加速: ${GREEN}$([ $USE_KVM -eq 1 ] && echo KVM || echo TCG)${NC}"
echo ""
echo -e "  端口转发:"
echo -e "    Web Dashboard → http://localhost:8080"
echo -e "    Monitor       → http://localhost:9090"
echo -e "    WebSocket     → ws://localhost:9998"
echo -e "    SSH           → ssh -p 2222 kai@localhost"
echo ""
echo "  启动中..."
echo ""

exec "$QEMU_BIN" "${QEMU_ARGS[@]}"
