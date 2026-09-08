# SPDX-License-Identifier: MIT
"""
tiered_sched.py — 分级调度 + 动态调优（ROADMAP 1.2）

支持：
  - 分级调度：简单规则 → Layer1，复杂决策 → Layer2
  - 调度器动态调优（根据负载自适应调整参数）
  - 分级阈值配置

用法：
    from tiered_sched import TieredScheduler
    ts = TieredScheduler()
    decision = ts.dispatch(features)
"""

import time
from typing import Dict, Any, Optional
from dataclasses import dataclass, field


@dataclass
class SchedTuning:
    """调度参数动态调优"""
    sched_latency_ns: int = 6000000       # 默认 6ms
    min_granularity_ns: int = 750000       # 默认 0.75ms
    wakeup_granularity_ns: int = 1000000   # 默认 1ms
    migration_cost_ns: int = 500000        # 默认 0.5ms

    def apply_load(self, load1: float, nr_cpu: int):
        """根据负载动态调优"""
        if load1 > nr_cpu * 0.8:
            # 高负载：增大调度延迟，减少迁移
            self.sched_latency_ns = min(self.sched_latency_ns * 2, 48000000)
            self.migration_cost_ns = self.migration_cost_ns * 2
        elif load1 < nr_cpu * 0.2:
            # 低负载：降低调度延迟，提高响应性
            self.sched_latency_ns = max(self.sched_latency_ns // 2, 2000000)
            self.migration_cost_ns = 250000
        # 否则保持不变


class TieredScheduler:
    """
    分级调度器

    策略：
      - Level 0: 简单规则（极低延迟，本地判断）
      - Level 1: 内核 AI（Layer1，微秒级）
      - Level 2: 外部 AI API（Layer2，毫秒级）
    """

    def __init__(self):
        self.tuning = SchedTuning()
        self._decision_cache: Dict[int, Dict] = {}
        self._stats = {"l0": 0, "l1": 0, "l2": 0}

    def _simple_rules(self, features: Dict) -> Optional[Dict]:
        """
        Level 0: 简单规则
        处理确定性、无歧义的情况
        """
        cpu_util = features.get("cpu_util", 0)
        prio = features.get("prio", 120)

        # 实时任务，直接升权
        if features.get("is_realtime"):
            return {"decision": "promote", "level": 0, "confidence": 95}

        # 空闲任务
        if features.get("is_idle"):
            return {"decision": "keep", "level": 0, "confidence": 90}

        # 优先级极高的前台任务
        if prio < 100 and cpu_util > 700:
            return {"decision": "promote", "level": 0, "confidence": 85}

        # 明显空闲
        if cpu_util < 50:
            return {"decision": "keep", "level": 0, "confidence": 90}

        return None  # 无法确定，进入下一级

    def _layer1_infer(self, features: Dict) -> Optional[Dict]:
        """
        Level 1: 内核 AI（启发式，微秒级）
        """
        cpu_util = features.get("cpu_util", 0)
        nvcsw = features.get("nvcsw", 0)
        nivcsw = features.get("nivcsw", 0)

        score = 0
        if cpu_util > 512: score += 30
        if nivcsw > 20: score += 25
        if nvcsw > 100: score += 15

        # 中等复杂度决策
        if score > 60:
            return {"decision": "promote", "level": 1,
                    "confidence": min(score, 90), "score": score}
        if score < 25:
            return {"decision": "demote", "level": 1,
                    "confidence": max(score, 20), "score": score}

        # 需要更复杂的判断
        return None

    def _layer2_infer(self, features: Dict) -> Dict:
        """
        Level 2: 外部 AI API（毫秒级）
        真实场景调用 DeepSeek/Kimi
        """
        # 模拟：返回 keep（真实场景调用 API）
        return {"decision": "keep", "level": 2, "confidence": 75}

    def dispatch(self, features: Dict) -> Dict:
        """分级分发调度决策"""
        # Level 0: 简单规则
        result = self._simple_rules(features)
        if result:
            self._stats["l0"] += 1
            return result

        # Level 1: 内核 AI
        result = self._layer1_infer(features)
        if result:
            self._stats["l1"] += 1
            return result

        # Level 2: 外部 AI
        result = self._layer2_infer(features)
        self._stats["l2"] += 1
        return result

    def dynamic_tune(self, load1: float, nr_cpu: int):
        """动态调优调度参数"""
        self.tuning.apply_load(load1, nr_cpu)
        return {
            "sched_latency_ns": self.tuning.sched_latency_ns,
            "min_granularity_ns": self.tuning.min_granularity_ns,
            "wakeup_granularity_ns": self.tuning.wakeup_granularity_ns,
            "migration_cost_ns": self.tuning.migration_cost_ns,
        }

    def stats(self) -> Dict:
        return dict(self._stats)


if __name__ == "__main__":
    ts = TieredScheduler()

    # 实时任务 → L0
    print(f"实时任务: {ts.dispatch({'is_realtime': True})}")

    # CPU 高 → L1
    print(f"CPU高: {ts.dispatch({'cpu_util': 700, 'nivcsw': 30, 'nvcsw': 120})}")

    # 复杂 → L2
    print(f"复杂: {ts.dispatch({'cpu_util': 400, 'nvcsw': 50})}")

    # 动态调优
    print(f"\n高负载调优: {ts.dynamic_tune(load1=8, nr_cpu=4)}")
    print(f"低负载调优: {ts.dynamic_tune(load1=0.5, nr_cpu=4)}")
    print(f"\n分级统计: {ts.stats()}")
