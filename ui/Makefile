# AI Linux UI — 构建系统
#
# 编译全部：
#   cd ai_linux/ui && make
#
# 编译单个：
#   make ai           # CLI
#   make ai-tui       # TUI（需 libncurses-dev）
#   make ai-web       # Web Dashboard

CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS := -lpthread -lm -lcurl
BUILD   := build

.PHONY: all clean help test

all: $(BUILD)/ai $(BUILD)/ai-web $(BUILD)/ai-tui $(BUILD)/ai-monitor
	@echo ""
	@echo "  UI 组件:"
	@echo "    CLI      ./build/ai --help"
	@echo "    TUI      ./build/ai-tui"
	@echo "    Web      ./build/ai-web --port 8080"
	@echo "    Monitor  ./build/ai-monitor --port 9090"
	@echo ""

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ai-monitor: web/ai_monitor.c
	@echo "  CC   web/ai_monitor.c"
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) 2>&1 | head -5 || true
	@if [ -f "$@" ]; then ls -lh $@; fi

$(BUILD):
	mkdir -p $(BUILD)

# CLI 主程序
$(BUILD)/ai: cli/ai_main.c
	@echo "  CC   cli/ai_main.c"
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lm 2>&1 | head -5 || true
	@if [ -f "$@" ]; then ls -lh $@; fi

# Web Dashboard
$(BUILD)/ai-web: web/ai_web.c
	@echo "  CC   web/ai_web.c"
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) 2>&1 | head -5 || true
	@if [ -f "$@" ]; then ls -lh $@; fi

# TUI 交互界面
$(BUILD)/ai-tui: tui/ai_tui.c
	@echo "  CC   tui/ai_tui.c (需要 apt install libncurses-dev)"
	$(CC) -O2 -Wall -o $@ $< -lncurses -lm 2>&1 | head -5
	@if [ -f "$@" ]; then ls -lh $@; fi

clean:
	rm -rf $(BUILD)
	@echo "  clean done"

test: $(BUILD)/ai
	@echo "  CLI 测试:"
	@$(BUILD)/ai status 2>&1 | head -20 || echo "  (需要 root)"

help:
	@echo ""
	@echo "  AI Linux UI 构建系统"
	@echo ""
	@echo "  编译:"
	@echo "    make all     全部编译"
	@echo "    make ai      CLI 主程序"
	@echo "    make ai-tui  TUI 交互界面"
	@echo "    make ai-web  Web Dashboard"
	@echo ""
	@echo "  运行示例:"
	@echo "    # TUI（推荐）"
	@echo "    DEEPSEEK_API_KEY=xxx ./build/ai-tui"
	@echo ""
	@echo "    # CLI"
	@echo "    ./build/ai ask '分析调度'"
	@echo "    ./build/ai sched --analyze"
	@echo "    ./build/ai security --full"
	@echo ""
	@echo "    # Web"
	@echo "    ./build/ai-web --port 8080 --key \$$DEEPSEEK_API_KEY"
	@echo "    # 浏览器打开 http://localhost:8080"
	@echo ""
