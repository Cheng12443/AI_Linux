# AI Linux Layer2 — 构建系统
#
# 编译：
#   cd layer2 && make
#
# 输出：
#   build/ai_layer2_gateway   — 主网关程序
#   build/libai_mcp.a         — MCP 库

CC       ?= gcc
CFLAGS   := -O2 -Wall -Wextra -I./include -I./mcp -I./skills -I./src
LDFLAGS  := -lpthread
BUILD    := build

MODULES  := $(BUILD)/ai_layer2_gateway

# 源文件
MCP_SRC   := mcp/ai_mcp_core.c
SKILL_SRC := skills/skill_registry.c
ROUTER_SRC:= src/ai_skill_router.c
GATEWAY_SRC:= src/ai_layer2_gateway.c
NETLINK_SRC:= src/ai_layer2_netlink.c
MEM_SRC   := ../ai_core/src/ai_memory.c

# 对象文件
MCP_OBJ   := $(BUILD)/ai_mcp_core.o
SKILL_OBJ:= $(BUILD)/skill_registry.o
ROUTER_OBJ:= $(BUILD)/ai_skill_router.o
NETLINK_OBJ:= $(BUILD)/ai_layer2_netlink.o

.PHONY: all clean test demo help

all: $(BUILD)/ai_layer2_gateway
	@echo "  构建完成: $(BUILD)/ai_layer2_gateway"
	@echo "  使用 --help 查看选项"

$(BUILD):
	mkdir -p $(BUILD)

# Step 1: MCP 核心库
$(MCP_OBJ): $(MCP_SRC) | $(BUILD)
	@echo "  CC  mcp/ai_mcp_core.c"
	$(CC) $(CFLAGS) -c $< -o $@

# Step 2: Skill 注册表
$(SKILL_OBJ): $(SKILL_SRC) | $(BUILD)
	@echo "  CC  skills/skill_registry.c"
	$(CC) $(CFLAGS) -c $< -o $@

# Step 3: Skill 路由引擎
$(ROUTER_OBJ): $(ROUTER_SRC) $(MCP_OBJ) $(SKILL_OBJ) | $(BUILD)
	@echo "  CC  src/ai_skill_router.c"
	$(CC) $(CFLAGS) -c $< -o $@

# Step 4: 完整网关程序
$(BUILD)/ai_layer2_gateway: $(GATEWAY_SRC) $(MCP_OBJ) $(SKILL_OBJ) $(ROUTER_OBJ)
	@echo "  LD  ai_layer2_gateway"
	$(CC) -o $@ \
		$(GATEWAY_SRC) \
		$(MCP_OBJ) \
		$(SKILL_OBJ) \
		$(ROUTER_OBJ) \
		$(LDFLAGS)
	@echo "  -> $@"
	@ls -lh $@

# 快速测试（演示模式，无需 API key）
demo: $(BUILD)/ai_layer2_gateway
	@echo ""
	@echo "  === 演示模式测试 ==="
	@echo ""
	@$(BUILD)/ai_layer2_gateway --demo --key demo
	@echo ""
	@echo "  演示完成！"
	@echo ""

# 语法检查
check:
	@echo "  语法检查..."
	@$(CC) $(CFLAGS) -fsyntax-only $(MCP_SRC) 2>&1 || true
	@$(CC) $(CFLAGS) -fsyntax-only $(SKILL_SRC) 2>&1 || true
	@$(CC) $(CFLAGS) -fsyntax-only $(ROUTER_SRC) 2>&1 || true
	@$(CC) $(CFLAGS) -fsyntax-only $(GATEWAY_SRC) 2>&1 || true
	@echo "  语法检查完成"

# 清理
clean:
	rm -rf $(BUILD)
	@echo "  clean done"

# 帮助
help:
	@echo ""
	@echo "  AI Linux Layer2 — 构建系统"
	@echo ""
	@echo "  目标："
	@echo "    make              编译完整网关"
	@echo "    make demo         编译并运行演示模式"
	@echo "    make check        语法检查"
	@echo "    make clean        清理构建目录"
	@echo ""
	@echo "  环境变量："
	@echo "    CC=gcc CFLAGS='...' make"
	@echo ""
	@echo "  运行时："
	@echo "    # DeepSeek"
	@echo "    DEEPSEEK_API_KEY=sk-xxx ./build/ai_layer2_gateway --mode async"
	@echo ""
	@echo "    # Kimi K3"
	@echo "    KIMI_API_KEY=sk-xxx ./build/ai_layer2_gateway --backend kimi --mode async"
	@echo ""
	@echo "    # 演示模式"
	@echo "    ./build/ai_layer2_gateway --demo --key demo"
	@echo ""
