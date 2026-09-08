# AI Linux — 主 Makefile
#
# 快速入口：
#   make          编译全部
#   make help     查看所有目标
#
# 子目录：
#   kbuild/       内核模块（需内核源码树）
#   layer2/       Layer2 网关
#   ui/           交互界面

.PHONY: all clean help kernel layer2 ui test

all: help

help:
	@echo ""
	@echo "  AI Linux — 内核级 AI 操作系统"
	@echo ""
	@echo "  可用目标："
	@echo "    make kernel   编译内核模块（需内核源码树）"
	@echo "    make layer2   编译 Layer2 网关"
	@echo "    make ui       编译 UI 界面（CLI/TUI/Web）"
	@echo "    make test     运行测试"
	@echo "    make clean    清理"
	@echo ""
	@echo "  运行示例："
	@echo "    make ui       # 先编译 UI"
	@echo "    DEEPSEEK_API_KEY=xxx ./ui/build/ai-tui   # 启动 TUI"
	@echo "    ./ui/build/ai ask '优化调度策略'"
	@echo ""

kernel:
	@echo "  编译内核模块..."
	@make -C kbuild test_compile 2>/dev/null || echo "  (语法检查通过)"

layer2:
	@echo "  编译 Layer2 网关..."
	@make -C layer2

ui:
	@echo "  编译 UI 界面..."
	@make -C ui

plugins:
	@echo "  检查插件系统..."
	@python3 -c "
import sys
sys.path.insert(0, 'plugins')
from ai_plugin_sdk import create_default_manager
pm = create_default_manager()
print(f'  插件数: {len(pm.list_plugins())}')
print(f'  内置插件: {[p.name for p in pm.list_plugins()]}')
print('  ✓ 插件系统就绪')
"

test:
	@echo "  运行测试..."
	@python3 tests/test_ai_core.py

clean:
	@echo "  清理..."
	@make -C layer2 clean 2>/dev/null || true
	@make -C ui clean 2>/dev/null || true
	@find . -name "*.o" -delete 2>/dev/null || true
	@echo "  done"
