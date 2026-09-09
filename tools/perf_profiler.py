#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
perf_profiler.py — AI Linux 性能分析与优化工具

功能：
  - 函数级性能分析（调用次数/总耗时/平均耗时/P99）
  - 热点路径识别
  - 内存使用分析
  - 优化建议生成

用法：
    python3 perf_profiler.py [--watch] [--report]
"""

import os
import sys
import time
import json
import statistics
from collections import defaultdict, OrderedDict
from typing import Dict, List, Callable, Any
from dataclasses import dataclass, field
from functools import wraps
import threading


# ============================================================================
# 性能数据收集
# ============================================================================

@dataclass
class FuncStats:
    """函数级统计"""
    name: str
    call_count: int = 0
    total_time_ms: float = 0.0
    min_time_ms: float = float('inf')
    max_time_ms: float = 0.0
    avg_time_ms: float = 0.0
    p50_time_ms: float = 0.0
    p95_time_ms: float = 0.0
    p99_time_ms: float = 0.0
    error_count: int = 0

    def update(self, elapsed_ms: float, error: bool = False):
        self.call_count += 1
        self.total_time_ms += elapsed_ms
        self.min_time_ms = min(self.min_time_ms, elapsed_ms)
        self.max_time_ms = max(self.max_time_ms, elapsed_ms)
        self.avg_time_ms = self.total_time_ms / self.call_count
        if error:
            self.error_count += 1


class Profiler:
    """
    性能分析器

    用法：
        profiler = Profiler()

        @profiler.profile
        def my_function():
            pass

        # 手动测量
        with profiler.measure("my_block"):
            pass

        # 获取报告
        report = profiler.report()
    """

    def __init__(self):
        self._stats: Dict[str, FuncStats] = {}
        self._times: Dict[str, List[float]] = defaultdict(list)
        self._lock = threading.Lock()
        self._enabled = True
        self._start_time = time.time()

    def profile(self, func: Callable) -> Callable:
        """装饰器：自动测量函数执行时间"""
        name = func.__qualname__

        @wraps(func)
        def wrapper(*args, **kwargs):
            if not self._enabled:
                return func(*args, **kwargs)

            start = time.perf_counter()
            error = None
            try:
                result = func(*args, **kwargs)
            except Exception as e:
                error = e
                raise
            finally:
                elapsed_ms = (time.perf_counter() - start) * 1000
                self._record(name, elapsed_ms, error is not None)

            return result

        return wrapper

    def measure(self, name: str):
        """上下文管理器：测量代码块"""
        return _MeasureContext(self, name)

    def _record(self, name: str, elapsed_ms: float, error: bool = False):
        with self._lock:
            if name not in self._stats:
                self._stats[name] = FuncStats(name=name)
            self._stats[name].update(elapsed_ms, error)
            self._times[name].append(elapsed_ms)

    def get_stats(self, name: str) -> FuncStats:
        """获取指定函数的统计"""
        with self._lock:
            return self._stats.get(name)

    def report(self, sort_by: str = "total") -> str:
        """
        生成性能报告

        sort_by: "total" / "count" / "avg" / "p99" / "error"
        """
        with self._lock:
            stats = list(self._stats.values())

        if sort_by == "total":
            stats.sort(key=lambda s: s.total_time_ms, reverse=True)
        elif sort_by == "count":
            stats.sort(key=lambda s: s.call_count, reverse=True)
        elif sort_by == "avg":
            stats.sort(key=lambda s: s.avg_time_ms, reverse=True)
        elif sort_by == "error":
            stats.sort(key=lambda s: s.error_count, reverse=True)
        else:
            stats.sort(key=lambda s: s.p99_time_ms, reverse=True)

        lines = [
            "",
            "=" * 90,
            "  AI Linux 性能分析报告",
            "=" * 90,
            "",
            f"{'函数名':<40} {'次数':>8} {'总计':>10} {'平均':>8} {'P99':>8} {'错误':>6}",
            "-" * 90,
        ]

        for s in stats[:30]:
            lines.append(
                f"{s.name:<40} {s.call_count:>8} "
                f"{s.total_time_ms:>9.1f}ms {s.avg_time_ms:>7.1f}ms "
                f"{s.p99_time_ms:>7.1f}ms {s.error_count:>6}"
            )

        # 汇总
        total_calls = sum(s.call_count for s in stats)
        total_time = sum(s.total_time_ms for s in stats)
        total_errors = sum(s.error_count for s in stats)
        uptime = time.time() - self._start_time

        lines.extend([
            "-" * 90,
            f"  总计: {total_calls} 次调用, {total_time:.1f}ms, "
            f"{total_errors} 错误, 运行 {uptime:.1f}s",
            "=" * 90,
            "",
        ])

        return "\n".join(lines)

    def hotspot_analysis(self) -> str:
        """热点路径分析"""
        with self._lock:
            stats = list(self._stats.values())

        if not stats:
            return "  暂无数据"

        stats.sort(key=lambda s: s.total_time_ms, reverse=True)

        lines = ["", "  热点路径 TOP 10:", ""]
        for i, s in enumerate(stats[:10]):
            pct = s.total_time_ms / max(sum(x.total_time_ms for x in stats), 1) * 100
            bar_len = int(pct / 2)
            bar = "█" * bar_len + "░" * (50 - bar_len)
            lines.append(
                f"  {i+1:>2}. {s.name:<40} {bar} {pct:5.1f}% "
                f"({s.call_count}次, {s.avg_time_ms:.1f}ms)"
            )

        return "\n".join(lines)

    def optimization_suggestions(self) -> List[str]:
        """生成优化建议"""
        with self._lock:
            stats = list(self._stats.values())

        suggestions = []

        for s in stats:
            # 高频 + 高延迟 = 优化目标
            if s.call_count > 100 and s.avg_time_ms > 10:
                suggestions.append(
                    f"  ⚠️  {s.name}: {s.call_count} 次调用，"
                    f"平均 {s.avg_time_ms:.1f}ms，建议优化"
                )

            # P99 异常高
            if s.p99_time_ms > s.avg_time_ms * 3 and s.p99_time_ms > 50:
                suggestions.append(
                    f"  ⚠️  {s.name}: P99={s.p99_time_ms:.0f}ms "
                    f"(平均{s.avg_time_ms:.1f}ms)，存在长尾延迟"
                )

            # 错误率高
            if s.error_count > 0 and s.call_count > 10:
                err_rate = s.error_count / s.call_count * 100
                if err_rate > 5:
                    suggestions.append(
                        f"  ⚠️  {s.name}: 错误率 {err_rate:.0f}% "
                        f"({s.error_count}/{s.call_count})"
                    )

        if not suggestions:
            suggestions.append("  ✅ 未发现明显性能问题")

        return suggestions


class _MeasureContext:
    def __init__(self, profiler: Profiler, name: str):
        self.profiler = profiler
        self.name = name

    def __enter__(self):
        self._start = time.perf_counter()
        return self

    def __exit__(self, *args):
        elapsed_ms = (time.perf_counter() - self._start) * 1000
        self.profiler._record(self.name, elapsed_ms)


# ============================================================================
# 全局 Profiler 实例
# ============================================================================

profiler = Profiler()


# ============================================================================
# 内存分析
# ============================================================================

class MemoryProfiler:
    """内存分析器"""

    @staticmethod
    def snapshot() -> Dict:
        """获取当前内存快照"""
        try:
            import resource
            usage = resource.getrusage(resource.RUSAGE_SELF)
            return {
                "max_rss_mb": usage.ru_maxrss / 1024,
                "page_faults": usage.ru_minflt + usage.ru_majflt,
                "swaps": usage.ru_nswap,
                "ctx_switches": usage.ru_nvcsw + usage.ru_nivcsw,
            }
        except Exception:
            return {}


# ============================================================================
# 演示
# ============================================================================

@profiler.profile
def simulate_ai_inference():
    """模拟 AI 推理"""
    time.sleep(0.001 + (os.getpid() % 10) * 0.001)


@profiler.profile
def simulate_sched_decision():
    """模拟调度决策"""
    time.sleep(0.0005)


@profiler.profile
def simulate_netlink_send():
    """模拟 netlink 发送"""
    time.sleep(0.0001)


def demo():
    """性能分析演示"""
    print("\n" + "=" * 60)
    print("  AI Linux 性能分析器")
    print("=" * 60)

    # 模拟工作负载
    print("\n  模拟工作负载...")
    for i in range(200):
        simulate_ai_inference()
        simulate_sched_decision()
        simulate_netlink_send()
        if i % 50 == 0:
            time.sleep(0.01)

    # 性能报告
    print(profiler.report())

    # 热点分析
    print(profiler.hotspot_analysis())

    # 优化建议
    print("\n  优化建议:")
    for s in profiler.optimization_suggestions():
        print(s)

    # 内存快照
    mem = MemoryProfiler.snapshot()
    if mem:
        print(f"\n  内存快照:")
        print(f"    RSS: {mem['max_rss_mb']:.1f} MB")
        print(f"    上下文切换: {mem['ctx_switches']}")

    print("\n  ✅ 分析完成\n")


if __name__ == "__main__":
    demo()
