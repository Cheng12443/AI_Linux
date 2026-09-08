#!/bin/bash
# ============================================================================
# KAI Linux — 容器启动脚本（完整版）
#
# 支持模式：
#   all       — 启动所有模块（supervisord 管理，默认）
#   gateway   — 仅 Layer2 网关
#   web       — 仅 Web Dashboard (8080)
#   monitor   — 仅性能监控 (9090)
#   ws        — 仅 WebSocket (9998)
#   shell     — 交互式 TUI
#   demo      — SDK 演示
#   perf      — 性能分析
#   help      — 帮助
# ============================================================================

set -euo pipefail

CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; NC='\033[0m'

echo ""
echo -e "${CYAN}╔═══════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║   KAI Linux — AI-powered Linux System        ║${NC}"
echo -e "${CYAN}║   v2.0.0 | DeepSeek + Kimi K3                 ║${NC}"
echo -e "${CYAN}╚═══════════════════════════════════════════════╝${NC}"
echo ""

# 显示配置
echo -e "${GREEN}配置:${NC}"
echo "  后端:     ${KAI_BACKEND:-deepseek}"
echo "  模式:     ${KAI_MODE:-async}"
echo "  DeepSeek: ${DEEPSEEK_API_KEY:+已设置(✓)}${DEEPSEEK_API_KEY:-未设置(✗)}"
echo "  Kimi K3:  ${KIMI_API_KEY:+已设置(✓)}${KIMI_API_KEY:-未设置(✗)}"
echo ""

if [ -z "$DEEPSEEK_API_KEY" ] && [ -z "$KIMI_API_KEY" ]; then
  echo -e "${YELLOW}⚠️  未设置 API 密钥，AI 推理功能不可用${NC}"
  echo -e "${YELLOW}   设置方式: -e DEEPSEEK_API_KEY=sk-xxx 或 -e KIMI_API_KEY=sk-xxx${NC}"
  echo ""
fi

MODE="${1:-all}"

case "$MODE" in
  all)
    echo -e "${GREEN}启动所有模块...${NC}"
    echo "  - Layer2 网关     (supervisor)"
    echo "  - Web Dashboard   (http://localhost:8080)"
    echo "  - 性能监控        (http://localhost:9090)"
    echo "  - WebSocket       (ws://localhost:9998)"
    echo ""
    exec /usr/bin/supervisord -c /etc/supervisor/conf.d/kai.conf
    ;;

  gateway)
    echo -e "${GREEN}启动 Layer2 网关...${NC}"
    cd /opt/kai-linux/layer2
    exec /usr/local/bin/kai-gateway --mode "${KAI_MODE:-async}"
    ;;

  web)
    echo -e "${GREEN}启动 Web Dashboard (8080)...${NC}"
    exec /usr/local/bin/kai-web --port 8080
    ;;

  monitor)
    echo -e "${GREEN}启动性能监控 (9090)...${NC}"
    exec /usr/local/bin/kai-monitor --port 9090
    ;;

  ws)
    echo -e "${GREEN}启动 WebSocket (9998)...${NC}"
    exec python3 /opt/kai-linux/sdk/python/ai_ws_server.py --port 9998
    ;;

  shell|tui)
    echo -e "${GREEN}启动 TUI 交互界面...${NC}"
    exec /usr/local/bin/kai-tui
    ;;

  demo)
    echo -e "${GREEN}运行 SDK 演示...${NC}"
    exec python3 /opt/kai-linux/sdk/python/demo.py
    ;;

  perf)
    echo -e "${GREEN}运行性能分析...${NC}"
    exec python3 /opt/kai-linux/tools/perf_profiler.py
    ;;

  help|--help|-h)
    echo "用法: docker run [OPTIONS] kai-linux [MODE]"
    echo ""
    echo "模式:"
    echo "  all       启动所有模块（默认）"
    echo "  gateway   仅 Layer2 网关"
    echo "  web       仅 Web Dashboard (8080)"
    echo "  monitor   仅性能监控 (9090)"
    echo "  ws        仅 WebSocket (9998)"
    echo "  shell     TUI 交互界面"
    echo "  demo      SDK 演示"
    echo "  perf      性能分析"
    ;;

  *)
    echo "未知模式: $MODE"
    echo "可用: all / gateway / web / monitor / ws / shell / demo / perf"
    exit 1
    ;;
esac
