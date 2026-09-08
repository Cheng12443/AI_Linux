#!/bin/bash
# ============================================================================
# KAI Linux — 可启动 ISO 构建脚本
#
# 生成标准 ISO 镜像，可在 VirtualBox / VMware / QEMU 中作为安装盘启动。
#
# 用法：
#   sudo ./build_iso.sh [--output kai-linux.iso]
#
# 依赖：
#   debootstrap genisoimage xorriso isolinux syslinux-utils
#
# 示例：
#   sudo ./build_iso.sh
# ============================================================================

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[0;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[iso]${NC} $*"; }
warn() { echo -e "${YELLOW}[iso]${NC} $*"; }
die()  { echo -e "${RED}[iso]${NC} $*" >&2; exit 1; }

OUTPUT="./kai-linux.iso"
DISTRO="ubuntu"
RELEASE="noble"
ARCH="amd64"
WORKDIR="/tmp/kai-iso-build"

while [[ $# -gt 0 ]]; do
  case $1 in
    --output) OUTPUT="$2"; shift 2 ;;
    --arch)   ARCH="$2"; shift 2 ;;
    *) die "未知参数: $1" ;;
  esac
done

[ "$EUID" -eq 0 ] || die "需要 root 权限"

# 依赖检查
for tool in debootstrap genisoimage xorriso; do
  command -v "$tool" >/dev/null 2>&1 || die "缺少依赖: $tool"
done

log "清理工作目录..."
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"/{rootfs,iso/{casper,install,boot/isolinux}}

# ============================================================================
# 构建 rootfs
# ============================================================================
log "构建 rootfs ($DISTRO $RELEASE $ARCH)..."
if [ "$DISTRO" = "debian" ]; then
  debootstrap --arch="$ARCH" "$RELEASE" "$WORKDIR/rootfs" http://deb.debian.org/debian
else
  debootstrap --arch="$ARCH" "$RELEASE" "$WORKDIR/rootfs" http://archive.ubuntu.com/ubuntu
fi

# 复制 KAI Linux
log "复制 KAI Linux 源码..."
mkdir -p "$WORKDIR/rootfs/opt/kai-linux"
cp -r "$(dirname "$0")/../.."/* "$WORKDIR/rootfs/opt/kai-linux/" 2>/dev/null || true

# 安装依赖（chroot）
log "安装依赖..."
mount --bind /dev "$WORKDIR/rootfs/dev"
mount --bind /proc "$WORKDIR/rootfs/proc"
mount --bind /sys "$WORKDIR/rootfs/sys"
trap 'umount "$WORKDIR/rootfs/dev" "$WORKDIR/rootfs/proc" "$WORKDIR/rootfs/sys" 2>/dev/null || true' EXIT

chroot "$WORKDIR/rootfs" /bin/bash -c "
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential gcc make python3 python3-pip \
  curl libncurses-dev clang 2>&1 | tail -2
apt-get clean
"

# ============================================================================
# 制作 squashfs
# ============================================================================
log "制作 squashfs..."
mksquashfs "$WORKDIR/rootfs" "$WORKDIR/iso/casper/filesystem.squashfs" \
  -comp xz -b 1M -noappend

# ============================================================================
# isolinux 引导配置
# ============================================================================
log "配置引导..."

cat > "$WORKDIR/iso/boot/isolinux/isolinux.cfg" << 'EOF'
DEFAULT live
LABEL live
  MENU LABEL KAI Linux (Live)
  KERNEL /casper/vmlinuz
  APPEND initrd=/casper/initrd boot=casper quiet splash
  TIMEOUT 30
EOF

# 复制内核和 initrd
cp "$WORKDIR/rootfs/boot/vmlinuz-"* "$WORKDIR/iso/casper/vmlinuz" 2>/dev/null || \
  warn "未找到 vmlinuz（可能在内核包中）"
cp "$WORKDIR/rootfs/boot/initrd.img-"* "$WORKDIR/iso/casper/initrd" 2>/dev/null || \
  warn "未找到 initrd.img"

# ============================================================================
# 生成 ISO
# ============================================================================
log "生成 ISO..."
xorriso -as mkisofs \
  -iso-level 3 \
  -full-iso9660-filenames \
  -volid "KAI_LINUX" \
  -output "$OUTPUT" \
  -isohybrid-mbr /usr/lib/ISOLINUX/isohdpfx.bin \
  -c boot/isolinux/boot.cat \
  -b boot/isolinux/isolinux.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  "$WORKDIR/iso"

# ============================================================================
# 完成
# ============================================================================
echo ""
echo "=============================================="
echo "  KAI Linux ISO 构建完成！"
echo "=============================================="
echo ""
echo "  ISO 文件: $OUTPUT"
echo "  大小:     $(ls -lh "$OUTPUT" | awk '{print $5}')"
echo ""
echo "  使用方式:"
echo "    VirtualBox: 新建虚拟机 → 挂载 ISO → 启动"
echo "    VMware:     新建虚拟机 → 挂载 ISO → 启动"
echo "    QEMU:       qemu-system-x86_64 -cdrom $OUTPUT -m 4096"
echo ""
