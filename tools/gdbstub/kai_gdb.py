# SPDX-License-Identifier: MIT
"""
kai_gdb.py — KAI Linux GDB 扩展（ROADMAP 7.4）

提供内核 AI 调试辅助命令：
  - kai-status: 查看 AI 子系统状态
  - kai-models: 列出加载的模型
  - kai-infer:  手动触发一次推理
  - kai-cache:  查看推理缓存统计
  - kai-metrics: 导出 Prometheus 指标

用法（在 GDB 内）：
  (gdb) source kai_gdb.py
  (gdb) kai-status
  (gdb) kai-infer 0x1234 256
"""

import gdb
import struct
import ctypes
from typing import Optional, Dict


class KaiStatusCommand(gdb.Command):
    """查看 AI 子系统状态"""

    def __init__(self):
        super().__init__("kai-status", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        gdb.write("=== KAI Linux 状态 ===\n")

        # 尝试读取全局上下文
        try:
            g_ctx = gdb.parse_and_eval("g_ctx")
            if g_ctx:
                total = g_ctx["total_inferences"]
                errors = g_ctx["total_errors"]
                gdb.write(f"  推理总数: {total}\n")
                gdb.write(f"  错误总数: {errors}\n")
        except Exception:
            gdb.write("  (g_ctx 不可读，模块可能未加载)\n")

        # 模型列表
        try:
            models = gdb.parse_and_eval("kai_models")
            gdb.write(f"  模型链表: {models}\n")
        except Exception:
            pass


class KaiInferCommand(gdb.Command):
    """手动触发推理
    用法: kai-infer <input_addr> <input_size>
    """

    def __init__(self):
        super().__init__("kai-infer", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        args = arg.split()
        if len(args) < 2:
            gdb.write("用法: kai-infer <input_addr> <input_size>\n")
            return

        addr = int(args[0], 0)
        size = int(args[1], 0)

        gdb.write(f"触发推理: input@{hex(addr)} size={size}\n")
        try:
            result = gdb.parse_and_eval(
                f"(int)kai_infer_simple(\"deepseek-chat\", "
                f"(void*){addr}, {size}, (void*)0x0, 0, 0)")
            gdb.write(f"  返回: {result}\n")
        except Exception as e:
            gdb.write(f"  调用失败: {e}\n")


class KaiModelsCommand(gdb.Command):
    """列出加载的模型"""

    def __init__(self):
        super().__init__("kai-models", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        gdb.write("=== 已加载模型 ===\n")
        try:
            models = gdb.parse_and_eval("kai_models")
            # 遍历链表（简化）
            gdb.write(f"  模型链头: {models}\n")
        except Exception as e:
            gdb.write(f"  (无法读取: {e})\n")


class KaiHelpCommand(gdb.Command):
    """KAI 调试命令帮助"""

    def __init__(self):
        super().__init__("kai-help", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        gdb.write("""
KAI Linux GDB 扩展命令:
  kai-status    查看 AI 子系统状态
  kai-models    列出加载的模型
  kai-infer     手动触发推理
  kai-help      本帮助

连接调试:
  1. 编译带调试符号:  make -C kai EXTRA_CFLAGS="-g"
  2. gdb vmlinux 或 gdb ./module.ko
  3. source kai_gdb.py
""")


def register_commands():
    """注册所有命令"""
    KaiStatusCommand()
    KaiInferCommand()
    KaiModelsCommand()
    KaiHelpCommand()
    gdb.write("KAI Linux GDB 扩展已加载。输入 kai-help 查看帮助。\n")


register_commands()
