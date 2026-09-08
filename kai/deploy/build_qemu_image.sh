#!/bin/bash
# ============================================================================
# KAI Linux — QEMU/KVM 磁盘镜像构建脚本
#
# 生成一个可在 QEMU / KVM 中启动的完整 Linux 磁盘镜像，
# 预装 KAI Linux 全套代码和依赖。
#
# 用法：
#   sudo ./build_qemu_image.sh [选项]
#
# 选项：
#   --size SIZE      镜像大小（默认 8G）
#   --distro NAME    发行版（默认 ubuntu，可选 debian）
#   --arch ARCH      架构（默认 amd64，可选 arm64）
#   --with-ui        预编译 UI（CLI/TUI/Web）
#   --with-kernel    尝试编译内核模块（需要内核头文件）
#   --output PATH    输出路径（默认 ./kai-linux.qcow2）
#
# 依赖：
#   qemu-img debootstrap qemu-user-static（跨架构时）
#
# 示例：
#   sudo ./build_qemu_image.sh --size 8G --with-ui
#   sudo ./build_qemu_image.sh --arch arm64 --distro debian
# ============================================================================

set -euo pipefail

# 颜色
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[0;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[build]${NC} $*"; }
warn() { echo -e "${YELLOW}[build]${NC} $*"; }
die()  { echo -e "${RED}[build]${NC} $*" >&2; exit 1; }

# ============================================================================
# 参数
# ============================================================================
IMG_SIZE="8G"
DISTRO="ubuntu"
RELEASE="noble"        # ubuntu 24.04
ARCH="amd64"
WITH_UI=0
WITH_KERNEL=0
OUTPUT="./kai-linux.qcow2"
MOUNT_POINT="/mnt/kai-build"

while [[ $# -gt 0 ]]; do
  case $1 in
    --size)       IMG_SIZE="$2"; shift 2 ;;
    --distro)     DISTRO="$2"; shift 2 ;;
    --arch)       ARCH="$2"; shift 2 ;;
    --with-ui)    WITH_UI=1; shift ;;
    --with-kernel) WITH_KERNEL=1; shift ;;
    --output)     OUTPUT="$2"; shift 2 ;;
    *) die "未知参数: $1" ;;
  esac
done

# ============================================================================
# 环境检查
# ============================================================================
log "检查依赖..."
for tool in qemu-img debootstrap; do
  command -v "$tool" >/dev/null 2>&1 || die "缺少依赖: $tool（apt install $tool）"
done

# 跨架构时检查 qemu-user-static
if [ "$ARCH" != "$(uname -m)" ]; then
  command -v qemu-aarch64-static >/dev/null 2>&1 || \
    warn "跨架构构建需要 qemu-user-static（apt install qemu-user-static）"
fi

[ "$EUID" -eq 0 ] || die "需要 root 权限（mount 操作）"

# ============================================================================
# 构建镜像
# ============================================================================
log "创建空白镜像 ($IMG_SIZE)..."
qemu-img create -f qcow2 "$OUTPUT" "$IMG_SIZE"

log "格式化文件系统..."
DEVICE=$(losetup -f --show "$OUTPUT")
trap 'losetup -d "$DEVICE" 2>/dev/null || true' EXIT

# 分区（单分区 + MBR）
parted -s "$DEVICE" mklabel msdos
parted -s "$DEVICE" mkpart primary ext4 1MiB 100%
parted -s "$DEVICE" set 1 boot on
mkfs.ext4 -F "${DEVICE}p1"

# 挂载
mkdir -p "$MOUNT_POINT"
mount "${DEVICE}p1" "$MOUNT_POINT"
trap 'umount "$MOUNT_POINT" 2>/dev/null || true; losetup -d "$DEVICE" 2>/dev/null || true' EXIT

# ============================================================================
# debootstrap 安装基础系统
# ============================================================================
log "安装基础系统 ($DISTRO $RELEASE $ARCH)..."

if [ "$DISTRO" = "debian" ]; then
  MIRROR="http://deb.debian.org/debian"
  RELEASE="${RELEASE:-bookworm}"
  debootstrap --arch="$ARCH" "$RELEASE" "$MOUNT_POINT" "$MIRROR"
else
  MIRROR="http://archive.ubuntu.com/ubuntu"
  debootstrap --arch="$ARCH" "$RELEASE" "$MOUNT_POINT" "$MIRROR"
fi

# ============================================================================
# 配置系统
# ============================================================================
log "配置系统..."

# 挂载虚拟文件系统
mount --bind /dev  "$MOUNT_POINT/dev"
mount --bind /proc "$MOUNT_POINT/proc"
mount --bind /sys  "$MOUNT_POINT/sys"
trap 'umount "$MOUNT_POINT/dev" "$MOUNT_POINT/proc" "$MOUNT_POINT/sys" 2>/dev/null || true; umount "$MOUNT_POINT" 2>/dev/null || true; losetup -d "$DEVICE" 2>/dev/null || true' EXIT

# 复制 KAI Linux 源码
log "复制 KAI Linux 源码..."
mkdir -p "$MOUNT_POINT/opt/kai-linux"
cp -r "$(dirname "$0")/../.."/* "$MOUNT_POINT/opt/kai-linux/" 2>/dev/null || true

# chroot 内安装依赖
log "在 chroot 中安装依赖..."
chroot "$MOUNT_POINT" /bin/bash -c "
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update

# 基础工具
apt-get install -y \
  build-essential gcc make curl wget \
  python3 python3-pip git vim \
  iproute2 net-tools iputils-ping \
  dnsutils tcpdump sysstat htop iotop \
  libncurses-dev clang llvm libbpf-dev bpftool \
  linux-tools-common linux-tools-generic \
  linux-headers-generic \
  liburing-dev libssl-dev libelf-dev bc kmod \
  2>&1 | tail -3

# Python 依赖
pip3 install --break-system-packages websockets pyyaml requests 2>/dev/null || \
pip3 install websockets pyyaml requests 2>/dev/null || true

# 清理
apt-get clean
rm -rf /var/lib/apt/lists/*
"

# 预编译 UI
if [ "$WITH_UI" -eq 1 ]; then
  log "预编译 UI..."
  chroot "$MOUNT_POINT" /bin/bash -c "
    cd /opt/kai-linux/ui && make ai-tui ai-web ai-monitor 2>/dev/null || true
    ln -sf /opt/kai-linux/ui/build/ai /usr/local/bin/ai 2>/dev/null || true
    ln -sf /opt/kai-linux/ui/build/ai-tui /usr/local/bin/kai-tui 2>/dev/null || true
    ln -sf /opt/kai-linux/ui/build/ai-web /usr/local/bin/kai-web 2>/dev/null || true
    ln -sf /opt/kai-linux/ui/build/ai-monitor /usr/local/bin/kai-monitor 2>/dev/null || true
  "
fi

# 预编译内核模块
if [ "$WITH_KERNEL" -eq 1 ]; then
  log "尝试编译内核模块..."
  chroot "$MOUNT_POINT" /bin/bash -c "
    cd /opt/kai-linux/kbuild
    make modules KERNEL_SRC=/usr/src/linux-headers-\$(uname -r) 2>&1 | tail -3 || \
      echo '内核模块编译需要与运行内核匹配的源码树'
  " || warn "内核模块编译失败（通常需要特定内核源码）"
fi

# ============================================================================
# 网络配置
# ============================================================================
log "配置网络..."

# DHCP
cat > "$MOUNT_POINT/etc/network/interfaces" << 'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF

# systemd-resolved
cat > "$MOUNT_POINT/etc/systemd/resolved.conf" << 'EOF'
[Resolve]
DNS=8.8.8.8 8.8.4.4
EOF

# hostname
echo "kai-linux" > "$MOUNT_POINT/etc/hostname"
cat > "$MOUNT_POINT/etc/hosts" << 'EOF'
127.0.0.1   localhost
127.0.1.1   kai-linux
EOF

# ============================================================================
# 用户配置
# ============================================================================
log "创建用户..."
chroot "$MOUNT_POINT" /bin/bash -c "
useradd -m -s /bin/bash kai 2>/dev/null || true
echo 'kai:kai123' | chpasswd
echo 'root:root123' | chpasswd
usermod -aG sudo kai 2>/dev/null || true
usermod -aG adm kai 2>/dev/null || true
"

# KAI 环境变量
cat > "$MOUNT_POINT/etc/kai-linux/env.sh" << 'EOF'
# KAI Linux 环境变量
export KAI_ROOT=/opt/kai-linux
export KAI_BACKEND=deepseek
export KAI_MODE=async
# 设置你的 API Key：
# export DEEPSEEK_API_KEY=sk-xxx
# export KIMI_API_KEY=sk-xxx
EOF

# ============================================================================
# GRUB 引导（需要额外处理，简化说明）
# ============================================================================
warn "注意: 脚本生成的镜像含完整 rootfs，但 GRUB 引导需要额外安装"
warn "最简单方式: 用 QEMU 直接以 kernel+initrd 启动，或用 extlinux"

# ============================================================================
# 清理
# ============================================================================
log "清理..."
umount "$MOUNT_POINT/dev" "$MOUNT_POINT/proc" "$MOUNT_POINT/sys" 2>/dev/null || true
umount "$MOUNT_POINT" 2>/dev/null || true
losetup -d "$DEVICE" 2>/dev/null || true
rmdir "$MOUNT_POINT" 2>/dev/null || true

# ============================================================================
# 完成
# ============================================================================
echo ""
echo "=============================================="
echo "  KAI Linux 镜像构建完成！"
echo "=============================================="
echo ""
echo "  镜像文件: $OUTPUT"
echo "  大小:     $(ls -lh "$OUTPUT" | awk '{print $5}')"
echo "  系统:     $DISTRO $RELEASE ($ARCH)"
echo ""
echo "  启动方式 (QEMU):"
echo "    qemu-system-x86_64 \\"
echo "      -drive file=$OUTPUT,format=qcow2 \\"
echo "      -m 4096 -smp 4 -net nic -net user \\"
echo "      -nographic"
echo ""
echo "  启动方式 (KVM 加速):"
echo "    qemu-system-x86_64 -enable-kvm \\"
echo "      -drive file=$OUTPUT,format=qcow2 \\"
echo "      -m 4096 -smp 4"
echo ""
echo "  登录: 用户名 kai 密码 kai123（或 root/root123）"
echo ""
