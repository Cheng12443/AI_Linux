#!/bin/bash
# ============================================================================
# KAI Linux — Docker 一键启动脚本
#
# 启动所有模块（主服务 + 各独立服务）
#
# 用法：
#   ./docker-up.sh                    # 构建并启动所有模块
#   ./docker-up.sh --build            # 强制重新构建镜像
#   ./docker-up.sh --full             # 启动完整服务组（含独立 gateway/web/monitor/ws）
#   ./docker-up.sh --down             # 停止并清理
#   ./docker-up.sh --logs             # 查看日志
#   ./docker-up.sh --status           # 查看状态
# ============================================================================

set -euo pipefail

CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; NC='\033[0m'

cd "$(dirname "$0")"

COMPOSE="docker compose"
if ! docker compose version >/dev/null 2>&1; then
  COMPOSE="docker-compose"
fi

do_build=0
do_full=0
do_down=0
do_logs=0
do_status=0

for arg in "$@"; do
  case $arg in
    --build)  do_build=1 ;;
    --full)   do_full=1 ;;
    --down)   do_down=1 ;;
    --logs)   do_logs=1 ;;
    --status) do_status=1 ;;
  esac
done

echo ""
echo -e "${CYAN}╔═══════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║   KAI Linux — Docker 一键启动                ║${NC}"
echo -e "${CYAN}╚═══════════════════════════════════════════════╝${NC}"
echo ""

# 检查 API Key
if [ -z "${DEEPSEEK_API_KEY:-}" ] && [ -z "${KIMI_API_KEY:-}" ]; then
  echo -e "${YELLOW}⚠️  未设置 API 密钥${NC}"
  echo -e "${YELLOW}   建议先设置:${NC}"
  echo -e "${YELLOW}     export DEEPSEEK_API_KEY=sk-xxx${NC}"
  echo -e "${YELLOW}     或${NC}"
  echo -e "${YELLOW}     export KIMI_API_KEY=sk-xxx${NC}"
  echo ""
fi

# 状态查询
if [ $do_status -eq 1 ]; then
  $COMPOSE ps
  exit 0
fi

# 停止
if [ $do_down -eq 1 ]; then
  echo -e "${GREEN}停止所有服务...${NC}"
  $COMPOSE down
  echo -e "${GREEN}已停止${NC}"
  exit 0
fi

# 日志
if [ $do_logs -eq 1 ]; then
  $COMPOSE logs -f --tail=100
  exit 0
fi

# 构建
if [ $do_build -eq 1 ] || ! docker image inspect kai-linux:latest >/dev/null 2>&1; then
  echo -e "${GREEN}构建镜像...${NC}"
  $COMPOSE build
  echo ""
fi

# 启动
echo -e "${GREEN}启动主服务（所有模块）...${NC}"
$COMPOSE up -d kai
echo ""

# 可选：启动完整服务组
if [ $do_full -eq 1 ]; then
  echo -e "${GREEN}启动完整服务组（gateway/web/monitor/ws 独立实例）...${NC}"
  $COMPOSE --profile full up -d
  echo ""
fi

# 等待健康
echo -e "${GREEN}等待服务就绪...${NC}"
for i in $(seq 1 15); do
  if docker inspect --format='{{.State.Health.Status}}' kai-linux 2>/dev/null | grep -q healthy; then
    echo -e "${GREEN}✓ 服务已就绪${NC}"
    break
  fi
  sleep 2
done

echo ""
echo -e "${CYAN}╔═══════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║   KAI Linux 已启动                          ║${NC}"
echo -e "${CYAN}╚═══════════════════════════════════════════════╝${NC}"
echo ""
echo -e "  访问地址:"
echo -e "    Web Dashboard  → ${GREEN}http://localhost:8080${NC}"
echo -e "    性能监控       → ${GREEN}http://localhost:9090${NC}"
echo -e "    WebSocket      → ${GREEN}ws://localhost:9998${NC}"
echo ""
echo -e "  常用命令:"
echo -e "    $COMPOSE logs -f              # 查看日志"
echo -e "    $COMPOSE ps                   # 查看状态"
echo -e "    $COMPOSE exec kai bash        # 进入容器"
echo -e "    $COMPOSE down                 # 停止"
echo ""
