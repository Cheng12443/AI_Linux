#!/bin/bash
# ============================================================================
# KAI Linux — Debian 打包脚本（ROADMAP 5.1）
#
# 生成 .deb 安装包。
#
# 用法：
#   ./debian_pack.sh
#
# 输出：
#   kai-linux_1.0.0_amd64.deb
# ============================================================================

set -euo pipefail

VERSION="1.0.0"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
PKG_NAME="kai-linux"
PKG_DIR="/tmp/kai-pkg"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

GREEN='\033[0;32m'; NC='\033[0m'
log() { echo -e "${GREEN}[deb]${NC} $*"; }

log "清理..."
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/DEBIAN"
mkdir -p "$PKG_DIR/opt/kai-linux"
mkdir -p "$PKG_DIR/usr/local/bin"
mkdir -p "$PKG_DIR/etc/kai-linux"
mkdir -p "$PKG_DIR/var/lib/kai-linux/models"
mkdir -p "$PKG_DIR/var/log/kai-linux"

# 复制源码
log "复制源码..."
cp -r "$ROOT"/* "$PKG_DIR/opt/kai-linux/" 2>/dev/null || true
rm -rf "$PKG_DIR/opt/kai-linux"/build "$PKG_DIR/opt/kai-linux"/__pycache__ 2>/dev/null || true

# 复制配置
cp "$ROOT/layer2/config/layer2.yaml" "$PKG_DIR/etc/kai-linux/config.yaml" 2>/dev/null || true

# control 文件
log "生成 control..."
cat > "$PKG_DIR/DEBIAN/control" << EOF
Package: $PKG_NAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: python3 (>= 3.8), python3-pip, curl, gcc, make
Maintainer: KAI Linux Team <kai@linux.ai>
Description: AI-powered Linux kernel system
 KAI Linux 将 AI 推理能力嵌入 Linux 内核，
 提供 AI 调度、网络异常检测、安全防护、内存预测等功能。
 支持 DeepSeek 和 Kimi K3 后端。
EOF

# postinst 脚本
log "生成 postinst..."
cat > "$PKG_DIR/DEBIAN/postinst" << 'EOF'
#!/bin/bash
set -e

# 安装 Python 依赖
pip3 install --break-system-packages websockets pyyaml requests 2>/dev/null || \
pip3 install websockets pyyaml requests 2>/dev/null || true

# 创建符号链接
for cmd in ai ai-tui ai-web ai-monitor; do
  [ -f "/opt/kai-linux/ui/build/$cmd" ] && \
    ln -sf "/opt/kai-linux/ui/build/$cmd" "/usr/local/bin/$cmd" || true
done

# 提示
echo ""
echo "KAI Linux 安装完成！"
echo "  配置 API Key:"
echo "    export DEEPSEEK_API_KEY=sk-xxx"
echo "    或"
echo "    export KIMI_API_KEY=sk-xxx"
echo "  启动:"
echo "    kai-tui   # TUI 界面"
echo "    kai-web --port 8080  # Web Dashboard"
echo ""

exit 0
EOF
chmod +x "$PKG_DIR/DEBIAN/postinst"

# postrm 脚本
cat > "$PKG_DIR/DEBIAN/postrm" << 'EOF'
#!/bin/bash
set -e
rm -f /usr/local/bin/ai /usr/local/bin/ai-tui \
      /usr/local/bin/ai-web /usr/local/bin/ai-monitor
exit 0
EOF
chmod +x "$PKG_DIR/DEBIAN/postrm"

# 构建 .deb
log "构建 .deb 包..."
dpkg-deb --build "$PKG_DIR" "${PKG_NAME}_${VERSION}_${ARCH}.deb"

log "完成: ${PKG_NAME}_${VERSION}_${ARCH}.deb"
ls -lh "${PKG_NAME}_${VERSION}_${ARCH}.deb"
