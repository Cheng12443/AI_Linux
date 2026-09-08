#!/bin/bash
# KAI Linux 安装脚本
#
# 用法：
#   curl -fsSL https://kai-linux.io/install.sh | bash
#   或
#   ./install.sh [--with-kernel] [--with-ui] [--with-sdk] [--all]

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[0;33m'
NC='\033[0m'

KAI_ROOT="/opt/kai-linux"
INSTALL_PREFIX="/usr/local"
WITH_KERNEL=0
WITH_UI=0
WITH_SDK=0
WITH_ALL=0

log()  { echo -e "${GREEN}[KAI]${NC} $*"; }
warn() { echo -e "${YELLOW}[KAI]${NC} $*"; }
error(){ echo -e "${RED}[KAI]${NC} $*" >&2; exit 1; }

usage() {
    echo "KAI Linux 安装脚本"
    echo ""
    echo "用法: $0 [选项]"
    echo ""
    echo "  --with-kernel   编译并安装内核模块"
    echo "  --with-ui       安装 UI（CLI/TUI/Web）"
    echo "  --with-sdk      安装 Python SDK"
    echo "  --all           全部安装"
    echo "  --help          帮助"
    echo ""
}

# 解析参数
for arg in "$@"; do
    case $arg in
        --with-kernel) WITH_KERNEL=1 ;;
        --with-ui)     WITH_UI=1 ;;
        --with-sdk)    WITH_SDK=1 ;;
        --all)         WITH_ALL=1 ;;
        --help)        usage; exit 0 ;;
    esac
done

[ $WITH_ALL -eq 1 ] && WITH_KERNEL=1 && WITH_UI=1 && WITH_SDK=1

echo ""
echo "  ╔═══════════════════════════════════════════╗"
echo "  ║   KAI Linux Installer                   ║"
echo "  ╚═══════════════════════════════════════════╝"
echo ""

# 检查 root
if [ "$EUID" -ne 0 ]; then
    warn "建议以 root 运行以获得完整功能"
fi

# 检查依赖
log "检查依赖..."
DEPS="gcc make python3 curl wget"
for dep in $DEPS; do
    if ! command -v $dep &>/dev/null; then
        error "缺少依赖: $dep"
    fi
done
log "依赖检查通过"

# 创建目录
log "创建目录..."
mkdir -p "$KAI_ROOT" \
         "$INSTALL_PREFIX/bin" \
         "/var/lib/kai-linux/models" \
         "/var/log/kai-linux" \
         "/etc/kai-linux"

# 复制文件
log "安装 KAI Linux..."
cp -r . "$KAI_ROOT/"

# 安装内核模块
if [ $WITH_KERNEL -eq 1 ]; then
    log "编译内核模块..."

    # 检查内核头文件
    if [ ! -d "/lib/modules/$(uname -r)/build" ]; then
        warn "内核头文件未安装，尝试安装..."
        if command -v apt-get &>/dev/null; then
            apt-get update && apt-get install -y linux-headers-$(uname -r)
        elif command -v yum &>/dev/null; then
            yum install -y kernel-devel-$(uname -r)
        else
            warn "请手动安装内核头文件"
        fi
    fi

    if [ -d "/lib/modules/$(uname -r)/build" ]; then
        cd "$KAI_ROOT/kbuild"
        make modules KERNEL_SRC="/lib/modules/$(uname -r)/build" 2>&1 | tail -5

        log "安装内核模块..."
        insmod "$KAI_ROOT/build/ai_core.ko" 2>/dev/null || warn "ai_core.ko 加载失败"
        insmod "$KAI_ROOT/build/kai_cache.ko" 2>/dev/null || warn "kai_cache.ko 加载失败"
        insmod "$KAI_ROOT/build/kai_infer.ko" 2>/dev/null || warn "kai_infer.ko 加载失败"

        lsmod | grep kai || true
    else
        warn "内核头文件不可用，跳过内核模块编译"
    fi
fi

# 安装 UI
if [ $WITH_UI -eq 1 ]; then
    log "编译 UI..."

    # ncurses
    if command -v apt-get &>/dev/null; then
        apt-get install -y libncurses-dev 2>/dev/null || true
    fi

    cd "$KAI_ROOT/ui"
    make ai-tui ai-web ai-monitor 2>/dev/null || true

    # 创建命令
    for cmd in ai ai-tui ai-web ai-monitor; do
        if [ -f "$KAI_ROOT/ui/build/$cmd" ]; then
            ln -sf "$KAI_ROOT/ui/build/$cmd" "$INSTALL_PREFIX/bin/$cmd"
            log "  安装命令: $cmd"
        fi
    done
fi

# 安装 SDK
if [ $WITH_SDK -eq 1 ]; then
    log "安装 Python SDK..."

    # Python 依赖
    pip3 install --break-system-packages websockets pyyaml 2>/dev/null || \
    pip3 install websockets pyyaml 2>/dev/null || true

    # 安装为 Python 包
    cd "$KAI_ROOT/sdk/python"
    pip3 install --break-system-packages . 2>/dev/null || true

    # 创建命令
    ln -sf "$KAI_ROOT/sdk/python/demo.py" "$INSTALL_PREFIX/bin/kai-demo"
    ln -sf "$KAI_ROOT/sdk/python/ai_ws_server.py" "$INSTALL_PREFIX/bin/kai-ws"
    ln -sf "$KAI_ROOT/tools/perf_profiler.py" "$INSTALL_PREFIX/bin/kai-perf"

    log "  安装命令: kai-demo, kai-ws, kai-perf"
fi

# 配置文件
log "安装配置..."
if [ ! -f "/etc/kai-linux/config.yaml" ]; then
    cp "$KAI_ROOT/layer2/config/layer2.yaml" "/etc/kai-linux/config.yaml"
    log "  配置文件: /etc/kai-linux/config.yaml"
fi

# 环境变量提示
log "设置环境变量..."
if [ -z "$DEEPSEEK_API_KEY" ] && [ -z "$KIMI_API_KEY" ]; then
    warn "请设置 API 密钥:"
    echo "  export DEEPSEEK_API_KEY=your_key"
    echo "  export KIMI_API_KEY=your_key"
    echo ""
fi

# 完成
echo ""
echo "  ╔═══════════════════════════════════════════╗"
echo "  ║   KAI Linux 安装完成！                    ║"
echo "  ╚═══════════════════════════════════════════╝"
echo ""
echo "  快速开始:"
echo "    kai-tui              # TUI 界面"
echo "    kai-web --port 8080 # Web Dashboard"
echo "    kai-monitor           # 性能监控"
echo "    kai-ws                # WebSocket 服务"
echo "    kai-demo              # SDK 演示"
echo "    kai-perf              # 性能分析"
echo ""
echo "  文档: $KAI_ROOT/README.md"
echo "  路线图: $KAI_ROOT/ROADMAP.md"
echo ""
