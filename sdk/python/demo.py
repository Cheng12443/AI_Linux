#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
ai_linux/demo.py — SDK 使用示例

运行：
    DEEPSEEK_API_KEY=xxx python3 demo.py
"""

import os
import sys
import time

# 添加 SDK 路径
sys.path.insert(0, os.path.dirname(__file__))

from ai_linux import AILinux, AIConfig


def demo_basic():
    """基础用法"""
    print("\n=== 基础用法 ===")

    ai = AILinux()
    response = ai.ask("用一句话介绍 Linux 内核调度器")
    print(f"AI: {response.text}")
    print(f"Backend: {response.backend}")
    print(f"延迟: {response.latency_ms:.1f}ms")


def demo_skill():
    """各 Skill 用法"""
    print("\n=== Skill 调用 ===")

    ai = AILinux()

    # 调度分析
    print("\n[调度分析]")
    r = ai.sched_analyze()
    print(f"AI: {r.text[:200]}...")

    # 安全扫描
    print("\n[安全检测]")
    r = ai.security_scan()
    print(f"AI: {r.text[:200]}...")

    # 网络分析
    print("\n[网络分析]")
    r = ai.net_analyze(connections=True)
    print(f"AI: {r.text[:200]}...")

    # 内存分析
    print("\n[内存分析]")
    r = ai.memory_analyze()
    print(f"AI: {r.text[:200]}...")

    # 代码生成
    print("\n[代码生成]")
    r = ai.code_generate(
        "监控系统 CPU、内存使用率，超过 80%% 告警",
        language="shell"
    )
    print(f"AI:\n{r.text[:300]}")


def demo_kimi():
    """Kimi K3 后端"""
    print("\n=== Kimi K3 后端 ===")

    api_key = os.getenv("KIMI_API_KEY")
    if not api_key:
        print("跳过（未设置 KIMI_API_KEY）")
        return

    ai = AILinux(backend="kimi")
    response = ai.ask("解释什么是 EEVDF 调度器")
    print(f"AI (Kimi): {response.text[:200]}...")
    print(f"延迟: {response.latency_ms:.1f}ms")


def demo_config():
    """配置管理"""
    print("\n=== 配置管理 ===")

    # 从环境变量
    cfg = AIConfig.from_env()
    print(f"后端: {cfg.backend.provider}")
    print(f"模型: {cfg.backend.model}")
    print(f"模式: {cfg.routing.mode}")
    print(f"调度阈值: {cfg.routing.sched_threshold}")
    print(f"API Key 环境变量: {cfg.backend.api_key_env}")

    # Skill 权重
    print("\nSkill 权重矩阵:")
    for w in cfg.skill_weights:
        print(f"  {w['skill']:<15} DeepSeek {w['deepseek']:>3}%  Kimi {w['kimi']:>3}%")

    # 保存到文件
    save_path = "/tmp/ai-linux-config-demo.yaml"
    cfg.save(save_path)
    print(f"\n配置已保存到: {save_path}")

    # 加载
    cfg2 = AIConfig.load(save_path)
    print(f"重新加载: backend={cfg2.backend.provider}")


def demo_batch():
    """批量请求"""
    print("\n=== 批量请求 ===")

    ai = AILinux()

    questions = [
        "CPU 使用率 100%% 怎么排查？",
        "内存泄漏如何检测？",
        "磁盘 IO 高的原因？",
    ]

    results = []
    for q in questions:
        r = ai.ask(q, use_cache=False)
        results.append(r)
        print(f"Q: {q[:30]}...")
        print(f"  A: {r.text[:60]}...")
        print(f"  延迟: {r.latency_ms:.0f}ms  后端: {r.backend}")
        print()


def demo_orchestrate():
    """编排任务"""
    print("\n=== 系统编排 ===")

    ai = AILinux()
    r = ai.orchestrate(
        "服务器响应变慢，从调度、内存、网络三个维度诊断并给出优化方案"
    )
    print(f"AI:\n{r.text[:400]}")


def main():
    print("╔══════════════════════════════════════════╗")
    print("║   AI Linux Python SDK Demo              ║")
    print("╚══════════════════════════════════════════╝")

    # 检查 API Key
    if not os.getenv("DEEPSEEK_API_KEY") and not os.getenv("KIMI_API_KEY"):
        print("⚠️  未设置 API 密钥")
        print("   请设置 DEEPSEEK_API_KEY 或 KIMI_API_KEY 环境变量")
        print()
        print("   仍然可以演示配置管理：")
        demo_config()
        return

    try:
        demo_basic()
        demo_skill()
        demo_kimi()
        demo_config()
        demo_orchestrate()

        print("\n✅ 全部演示完成！")

    except Exception as e:
        print(f"\n❌ 错误: {e}")
        import traceback
        traceback.print_exc()


if __name__ == "__main__":
    main()
