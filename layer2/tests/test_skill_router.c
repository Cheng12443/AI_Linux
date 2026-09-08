#!/bin/bash
# Layer2 Skill 路由测试脚本

set -e
cd "$(dirname "$0")/.."

echo "=========================================="
echo "  AI Linux Layer2 — Skill 路由测试"
echo "=========================================="

# 检查编译产物
if [ ! -f "build/ai_layer2_gateway" ]; then
    echo "  编译中..."
    make
fi

echo ""
echo "--- Test 1: DeepSeek 后端调度请求路由 ---"
./build/ai_layer2_gateway --demo --key test \
    --backend deepseek \
    2>&1 | grep -E "后端|Backend|DeepSeek|Kimi" | head -5

echo ""
echo "--- Test 2: Kimi 后端安全请求路由 ---"
./build/ai_layer2_gateway --demo --key test \
    --backend kimi \
    2>&1 | grep -E "后端|Backend|DeepSeek|Kimi" | head -5

echo ""
echo "--- Test 3: Skill 关键词识别测试 ---"
cat << 'EOF' | ./build/ai_layer2_gateway --demo --key test 2>&1 | grep -E "Skill|skill|category|路由" | head -10

分析进程1234的CPU使用，判断是否需要调度升权

EOF

echo ""
echo "--- Test 4: MCP 工具列表请求 ---"
cat << 'EOF' > /tmp/mcp_test.json
{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}
EOF
echo "MCP tools/list 请求已就绪（需网关支持 MCP 模式）"

echo ""
echo "--- Test 5: 编译产物检查 ---"
if [ -f "build/ai_layer2_gateway" ]; then
    size=$(stat -c%s build/ai_layer2_gateway 2>/dev/null || stat -f%z build/ai_layer2_gateway 2>/dev/null)
    echo "  ✓ ai_layer2_gateway 存在 ($(numfmt --to=iec $size 2>/dev/null || echo "${size} bytes"))"
else
    echo "  ✗ 编译产物不存在"
fi

if [ -f "build/ai_mcp_core.o" ]; then
    echo "  ✓ MCP 核心库已编译"
fi

echo ""
echo "--- Test 6: Skill 路由代码检查 ---"
for keyword in "scheduling_expert" "network_io_expert" "security_expert" "memory_expert"; do
    if grep -q "$keyword" skills/skill_registry.c; then
        echo "  ✓ Skill: $keyword"
    else
        echo "  ✗ 缺失: $keyword"
    fi
done

echo ""
echo "--- Test 7: MCP 工具检查 ---"
for tool in "sched_analyze" "io_inspect" "security_scan" "memory_predict" "sys_info"; do
    if grep -q "\"$tool\"" skills/skill_registry.c; then
        echo "  ✓ MCP 工具: $tool"
    else
        echo "  ✗ 缺失: $tool"
    fi
done

echo ""
echo "--- Test 8: Skill 路由关键词匹配 ---"
for rule in "调度" "网络" "安全" "内存" "代码" "编排"; do
    if grep -q "\"$rule\"" src/ai_skill_router.c; then
        echo "  ✓ 关键词: \"$rule\""
    else
        echo "  ✗ 缺失: \"$rule\""
    fi
done

echo ""
echo "=========================================="
echo "  测试完成"
echo "=========================================="
