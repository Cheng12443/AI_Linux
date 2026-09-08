# SPDX-License-Identifier: MIT
"""
persistence.py — 推理结果持久化（ROADMAP 2.1）

支持：
  - 决策历史记录到磁盘
  - JSON Lines 格式（追加写）
  - 查询 / 过滤
  - 自动轮转

用法：
    from persistence import DecisionStore
    ds = DecisionStore("/var/lib/kai-linux/decisions.jsonl")
    ds.record(decision="promote", confidence=85, domain="sched")
    recent = ds.query(domain="sched", limit=10)
"""

import os
import json
import time
import threading
from typing import Dict, List, Any, Optional
from dataclasses import dataclass, asdict


@dataclass
class Decision:
    request_id: str
    domain: str            # sched / io / security / memory
    decision: str
    confidence: int
    reason: str = ""
    backend: str = ""
    model: str = ""
    latency_ms: float = 0.0
    timestamp: float = 0.0
    metadata: Dict = None

    def to_dict(self) -> Dict:
        d = asdict(self)
        if d["metadata"] is None:
            d["metadata"] = {}
        return d


class DecisionStore:
    """
    推理决策持久化存储（JSON Lines）
    """

    def __init__(self, path: str, max_size_mb: int = 100):
        self.path = path
        self.max_size_mb = max_size_mb
        self._lock = threading.Lock()
        os.makedirs(os.path.dirname(path), exist_ok=True)

    def record(self, decision: str, confidence: int,
               domain: str = "sched",
               request_id: Optional[str] = None,
               reason: str = "", backend: str = "",
               model: str = "", latency_ms: float = 0.0,
               metadata: Optional[Dict] = None) -> Decision:
        """记录一条决策"""
        import uuid
        d = Decision(
            request_id=request_id or uuid.uuid4().hex[:16],
            domain=domain,
            decision=decision,
            confidence=confidence,
            reason=reason,
            backend=backend,
            model=model,
            latency_ms=latency_ms,
            timestamp=time.time(),
            metadata=metadata,
        )
        with self._lock:
            with open(self.path, "a") as f:
                f.write(json.dumps(d.to_dict(), ensure_ascii=False) + "\n")
            self._maybe_rotate()
        return d

    def _maybe_rotate(self):
        """检查文件大小，超限则轮转"""
        try:
            size_mb = os.path.getsize(self.path) / (1024 * 1024)
            if size_mb > self.max_size_mb:
                rotated = f"{self.path}.{int(time.time())}"
                os.rename(self.path, rotated)
        except Exception:
            pass

    def query(self, domain: Optional[str] = None,
              decision: Optional[str] = None,
              min_confidence: int = 0,
              limit: int = 100) -> List[Dict]:
        """查询决策（最近 N 条，倒序）"""
        results = []
        try:
            with open(self.path) as f:
                lines = f.readlines()
            for line in reversed(lines):
                try:
                    d = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if domain and d.get("domain") != domain:
                    continue
                if decision and d.get("decision") != decision:
                    continue
                if d.get("confidence", 0) < min_confidence:
                    continue
                results.append(d)
                if len(results) >= limit:
                    break
        except FileNotFoundError:
            pass
        return results

    def stats(self) -> Dict:
        """统计决策分布"""
        stats = {"total": 0, "domains": {}, "decisions": {}}
        try:
            with open(self.path) as f:
                for line in f:
                    try:
                        d = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    stats["total"] += 1
                    dom = d.get("domain", "unknown")
                    dec = d.get("decision", "unknown")
                    stats["domains"][dom] = stats["domains"].get(dom, 0) + 1
                    stats["decisions"][dec] = stats["decisions"].get(dec, 0) + 1
        except FileNotFoundError:
            pass
        return stats


if __name__ == "__main__":
    ds = DecisionStore("/tmp/kai_decisions.jsonl")
    ds.record(decision="promote", confidence=85, domain="sched", backend="deepseek")
    ds.record(decision="keep", confidence=60, domain="sched", backend="kimi")
    ds.record(decision="drop", confidence=90, domain="io")

    print("统计:", ds.stats())
    print("\n最近调度决策:")
    for d in ds.query(domain="sched", limit=5):
        print(f"  {d['decision']} ({d['confidence']}%) via {d['backend']}")
