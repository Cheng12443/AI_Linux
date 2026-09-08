# SPDX-License-Identifier: MIT
"""
load_balancer.py — 多 API 负载均衡 + 故障转移（ROADMAP 2.2）

支持：
  - DeepSeek / Kimi 自动故障转移
  - 轮询 / 加权 / 最少连接 策略
  - 健康检查 + 自动摘除故障后端
  - 熔断恢复

用法：
    lb = LoadBalancer()
    lb.add_backend("deepseek", weight=60)
    lb.add_backend("kimi", weight=40)
    result = lb.infer("分析负载")
"""

import os
import time
import random
import threading
from typing import Dict, List, Any, Optional
from dataclasses import dataclass, field


@dataclass
class Backend:
    name: str
    weight: int = 50
    healthy: bool = True
    failures: int = 0
    successes: int = 0
    last_failure_ns: float = 0.0
    cooldown_ns: float = 0.0
    active_connections: int = 0
    total_requests: int = 0
    total_latency_ms: float = 0.0

    @property
    def avg_latency_ms(self) -> float:
        return self.total_latency_ms / max(self.total_requests, 1)

    def to_dict(self) -> Dict:
        return {
            "name": self.name,
            "weight": self.weight,
            "healthy": self.healthy,
            "failures": self.failures,
            "successes": self.successes,
            "active_connections": self.active_connections,
            "total_requests": self.total_requests,
            "avg_latency_ms": round(self.avg_latency_ms, 2),
        }


class LoadBalancer:
    """
    多后端负载均衡器

    策略：
      - round_robin: 轮询
      - weighted: 加权轮询
      - least_conn: 最少连接
      - random: 随机
    """

    def __init__(self, strategy: str = "weighted",
                 failure_threshold: int = 5,
                 cooldown_ms: int = 30000):
        self.strategy = strategy
        self.failure_threshold = failure_threshold
        self.cooldown_ms = cooldown_ms
        self._backends: Dict[str, Backend] = {}
        self._rr_index = 0
        self._lock = threading.Lock()

    def add_backend(self, name: str, weight: int = 50):
        """添加后端"""
        with self._lock:
            self._backends[name] = Backend(name=name, weight=weight)

    def remove_backend(self, name: str):
        """移除后端"""
        with self._lock:
            self._backends.pop(name, None)

    def mark_success(self, name: str, latency_ms: float = 0):
        """标记成功"""
        with self._lock:
            b = self._backends.get(name)
            if b:
                b.successes += 1
                b.failures = 0
                b.healthy = True
                b.total_requests += 1
                b.total_latency_ms += latency_ms
                b.active_connections = max(0, b.active_connections - 1)

    def mark_failure(self, name: str):
        """标记失败"""
        with self._lock:
            b = self._backends.get(name)
            if b:
                b.failures += 1
                b.total_requests += 1
                b.active_connections = max(0, b.active_connections - 1)
                if b.failures >= self.failure_threshold:
                    b.healthy = False
                    b.last_failure_ns = time.time()
                    b.cooldown_ns = self.cooldown_ms / 1000.0

    def _is_healthy(self, b: Backend) -> bool:
        """检查后端是否健康（含冷却恢复）"""
        if b.healthy:
            return True
        # 冷却期过了，尝试恢复
        if time.time() - b.last_failure_ns > b.cooldown_ns:
            b.healthy = True
            b.failures = 0
            return True
        return False

    def select_backend(self) -> Optional[str]:
        """选择后端"""
        with self._lock:
            healthy = [b for b in self._backends.values() if self._is_healthy(b)]
            if not healthy:
                return None

            if self.strategy == "round_robin":
                return self._round_robin(healthy)
            elif self.strategy == "least_conn":
                return self._least_conn(healthy)
            elif self.strategy == "random":
                return random.choice(healthy).name
            else:  # weighted
                return self._weighted(healthy)

    def _round_robin(self, healthy: List[Backend]) -> str:
        b = healthy[self._rr_index % len(healthy)]
        self._rr_index += 1
        return b.name

    def _least_conn(self, healthy: List[Backend]) -> str:
        b = min(healthy, key=lambda x: x.active_connections)
        b.active_connections += 1
        return b.name

    def _weighted(self, healthy: List[Backend]) -> str:
        total = sum(b.weight for b in healthy)
        r = random.uniform(0, total)
        upto = 0
        for b in healthy:
            upto += b.weight
            if r <= upto:
                b.active_connections += 1
                return b.name
        b = healthy[0]
        b.active_connections += 1
        return b.name

    def infer(self, prompt: str,
              infer_fn: Optional[Any] = None) -> Dict:
        """
        执行推理，自动故障转移
        """
        backend = self.select_backend()
        if not backend:
            return {"error": "无可用后端", "backend": None}

        # 实际调用（由调用方提供 infer_fn，或使用 SDK）
        if infer_fn:
            try:
                result = infer_fn(backend, prompt)
                self.mark_success(backend, result.get("latency_ms", 0))
                return result
            except Exception as e:
                self.mark_failure(backend)
                # 故障转移
                fallback = self.select_backend()
                if fallback and fallback != backend:
                    try:
                        result = infer_fn(fallback, prompt)
                        self.mark_success(fallback, result.get("latency_ms", 0))
                        result["fallback_from"] = backend
                        return result
                    except Exception:
                        self.mark_failure(fallback)
                return {"error": str(e), "backend": backend}

        return {"backend": backend, "prompt": prompt}

    def status(self) -> List[Dict]:
        """获取所有后端状态"""
        with self._lock:
            return [b.to_dict() for b in self._backends.values()]

    def stats(self) -> Dict:
        """获取汇总统计"""
        with self._lock:
            total_req = sum(b.total_requests for b in self._backends.values())
            total_fail = sum(b.failures for b in self._backends.values())
            healthy = sum(1 for b in self._backends.values() if b.healthy)
            return {
                "total_backends": len(self._backends),
                "healthy_backends": healthy,
                "total_requests": total_req,
                "total_failures": total_fail,
                "error_rate": round(total_fail / max(total_req, 1) * 100, 2),
            }


if __name__ == "__main__":
    lb = LoadBalancer(strategy="weighted")
    lb.add_backend("deepseek", weight=60)
    lb.add_backend("kimi", weight=40)

    print("后端状态:")
    for s in lb.status():
        print(f"  {s['name']}: weight={s['weight']} healthy={s['healthy']}")

    print(f"\n汇总: {lb.stats()}")
