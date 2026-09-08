#!/bin/bash
# AI Linux Layer2 Gateway 启动脚本
#
# 用法：
#   source layer2.env.sh  # 先加载环境变量
#   ./start_layer2.sh
#
# 或一键：
#   DEEPSEEK_API_KEY=sk-xxx ./start_layer2.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BINARY="$PROJECT_ROOT/build/ai_layer2_gateway"

# 加载环境变量
if [ -f "$SCRIPT_DIR/layer2.env.sh" ]; then
    source "$SCRIPT_DIR/layer2.env.sh"
fi

# 检查 API Key
if [ -z "$DEEPSEEK_API_KEY" ] && [ -z "$KIMI_API_KEY" ]; then
    echo "错误: 未设置 API 密钥"
    echo ""
    echo "请设置以下环境变量之一："
    echo "  export DEEPSEEK_API_KEY=your_key"
    echo "  export KIMI_API_KEY=your_key"
    echo ""
    echo "或编辑 $SCRIPT_DIR/layer2.env.sh"
    echo ""
    echo "运行演示模式（无需密钥）："
    echo "  ./build/ai_layer2_gateway --demo --key demo"
    exit 1
fi

# 选择后端
BACKEND_ARG=""
if [ -n "$KIMI_API_KEY" ] && [ -z "$DEEPSEEK_API_KEY" ]; then
    BACKEND_ARG="--backend kimi"
elif [ -n "$KIMI_API_KEY" ]; then
    BACKEND_ARG="--backend ${AI_BACKEND:-deepseek}"
fi

# 构建参数
ARGS=(
    --backend    "${AI_BACKEND:-deepseek}"
    --key        "${DEEPSEEK_API_KEY:-$KIMI_API_KEY}"
    --model      "${AI_MODEL:-}"
    --mode       "${AI_MODE:-async}"
    --sched-threshold "${AI_THRESHOLD_SCHED:-7000}"
    --io-threshold    "${AI_THRESHOLD_IO:-7000}"
    --sec-threshold   "${AI_THRESHOLD_SEC:-8000}"
    --timeout    "${AI_API_TIMEOUT_MS:-5000}"
)

# 调试模式
if [ "${AI_LOG_LEVEL:-info}" = "debug" ]; then
    ARGS+=(--debug)
fi

echo "========================================"
echo "  AI Linux Layer2 Gateway"
echo "  Backend: ${AI_BACKEND:-deepseek}"
echo "  Mode:    ${AI_MODE:-async}"
echo "========================================"

# 执行
exec "$BINARY" "${ARGS[@]}"
