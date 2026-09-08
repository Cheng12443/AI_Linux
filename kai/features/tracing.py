# SPDX-License-Identifier: MIT
"""
tracing.py — 分布式追踪 + 火焰图 + BPF Tracing（ROADMAP 6.2）

支持：
  - 分布式追踪（跟踪推理请求完整链路）
  - 性能火焰图（可视化热点）
  - tracepoint 自动发现
  - BPF Tracing 辅助

用法：
    from tracing import Tracer, FlameGraph
    tracer = Tracer()
    with tracer.span("inference"):
        # ... 推理逻辑
    tracer.report()
"""

import time
import json
import uuid
import threading
from typing import Dict, List, Any, Optional
from dataclasses import dataclass, field


@dataclass
class Span:
    """追踪跨度"""
    name: str
    span_id: str
    parent_id: Optional[str]
    trace_id: str
    start_time: float
    end_time: float = 0.0
    duration_ms: float = 0.0
    metadata: Dict = field(default_factory=dict)

    def to_dict(self) -> Dict:
        return {
            "name": self.name,
            "span_id": self.span_id,
            "parent_id": self.parent_id,
            "trace_id": self.trace_id,
            "start_time": self.start_time,
            "duration_ms": self.duration_ms,
            "metadata": self.metadata,
        }


class Tracer:
    """
    分布式追踪器（简化的 OpenTelemetry 风格）
    """

    def __init__(self):
        self._spans: List[Span] = []
        self._current: Dict[int, Optional[str]] = {}  # thread_id -> span_id
        self._lock = threading.Lock()
        self._local = threading.local()

    def start_trace(self, name: str, **metadata) -> Span:
        """开始一个 trace"""
        trace_id = uuid.uuid4().hex[:16]
        span = Span(
            name=name,
            span_id=uuid.uuid4().hex[:8],
            parent_id=None,
            trace_id=trace_id,
            start_time=time.time(),
            metadata=metadata,
        )
        self._local.current_span = span.span_id
        with self._lock:
            self._spans.append(span)
        return span

    def span(self, name: str, **metadata):
        """上下文管理器：创建一个 span"""
        return _SpanContext(self, name, metadata)

    def _begin_span(self, name: str, metadata: Dict) -> Span:
        parent_id = getattr(self._local, "current_span", None)
        trace_id = getattr(self._local, "current_trace", None) or uuid.uuid4().hex[:16]

        span = Span(
            name=name,
            span_id=uuid.uuid4().hex[:8],
            parent_id=parent_id,
            trace_id=trace_id,
            start_time=time.time(),
            metadata=metadata,
        )
        self._local.current_span = span.span_id
        self._local.current_trace = trace_id
        with self._lock:
            self._spans.append(span)
        return span

    def _end_span(self, span: Span):
        span.end_time = time.time()
        span.duration_ms = (span.end_time - span.start_time) * 1000
        # 恢复父 span
        parent = span.parent_id
        self._local.current_span = parent

    def get_spans(self, trace_id: Optional[str] = None) -> List[Dict]:
        """获取 span"""
        with self._lock:
            spans = self._spans
            if trace_id:
                spans = [s for s in spans if s.trace_id == trace_id]
            return [s.to_dict() for s in spans]

    def report(self, sort_by: str = "duration") -> str:
        """生成追踪报告"""
        spans = sorted(self._spans, key=lambda s: s.duration_ms, reverse=True)
        lines = ["", "=" * 70, "  分布式追踪报告", "=" * 70, ""]
        lines.append(f"{'Span':<30} {'耗时(ms)':>10} {'Trace':<18}")
        lines.append("-" * 70)
        for s in spans[:30]:
            lines.append(f"{s.name:<30} {s.duration_ms:>10.2f} {s.trace_id:<18}")
        return "\n".join(lines)

    def export_json(self, path: str):
        """导出为 JSON（可导入 Jaeger/Zipkin）"""
        data = [s.to_dict() for s in self._spans]
        with open(path, "w") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)


class _SpanContext:
    def __init__(self, tracer: Tracer, name: str, metadata: Dict):
        self.tracer = tracer
        self.name = name
        self.metadata = metadata
        self.span = None

    def __enter__(self):
        self.span = self.tracer._begin_span(self.name, self.metadata)
        return self

    def __exit__(self, *args):
        self.tracer._end_span(self.span)


class FlameGraph:
    """
    性能火焰图生成器
    """

    def __init__(self):
        self._samples: List[tuple] = []  # (stack, count)

    def sample(self, stack: List[str], count: int = 1):
        """采样调用栈"""
        self._samples.append((tuple(stack), count))

    def generate(self, output_path: str = "/tmp/kai-flame.txt"):
        """生成 flamegraph.pl 格式数据"""
        import collections
        counts = collections.Counter()
        for stack, count in self._samples:
            for i in range(len(stack)):
                counts[";".join(stack[:i+1])] += count

        with open(output_path, "w") as f:
            for stack_str, count in counts.items():
                f.write(f"{stack_str} {count}\n")
        return output_path

    def summary(self) -> str:
        """生成热点摘要"""
        import collections
        leaf_counts = collections.Counter()
        for stack, count in self._samples:
            if stack:
                leaf_counts[stack[-1]] += count

        total = sum(leaf_counts.values()) or 1
        lines = ["", "=== 热点函数 ==="]
        for func, count in leaf_counts.most_common(10):
            pct = count * 100 / total
            bar = "█" * int(pct / 2)
            lines.append(f"  {func:<30} {bar} {pct:.1f}%")
        return "\n".join(lines)


class TracepointDiscovery:
    """
    tracepoint 自动发现
    """

    @staticmethod
    def discover(kernel_trace_dir: str = "/sys/kernel/debug/tracing/events") -> List[str]:
        """发现可用的 tracepoint"""
        import os
        tracepoints = []
        try:
            for root, dirs, files in os.walk(kernel_trace_dir):
                if "format" in files:
                    # 提取子系统:事件
                    rel = os.path.relpath(root, kernel_trace_dir)
                    if rel != ".":
                        tracepoints.append(rel.replace(os.sep, ":"))
        except Exception:
            pass
        return sorted(tracepoints)

    @staticmethod
    def discover_bpf() -> List[str]:
        """发现 BPF tracepoint（需 bpftrace）"""
        import subprocess
        try:
            result = subprocess.run(
                ["bpftrace", "-l", "tracepoint:*"],
                capture_output=True, text=True, timeout=10,
            )
            return [l for l in result.stdout.split("\n") if l]
        except Exception:
            return []


if __name__ == "__main__":
    tracer = Tracer()

    tracer.start_trace("sched_inference")
    with tracer.span("collect_features"):
        time.sleep(0.01)
    with tracer.span("ai_inference"):
        time.sleep(0.05)
        with tracer.span("api_call"):
            time.sleep(0.03)
    with tracer.span("apply_decision"):
        time.sleep(0.005)

    print(tracer.report())

    # 火焰图
    fg = FlameGraph()
    fg.sample(["main", "infer", "api_call", "http_post"], 10)
    fg.sample(["main", "infer", "cache_lookup"], 30)
    fg.sample(["main", "infer", "local_model"], 20)
    print(fg.summary())

    # tracepoint 发现
    tp = TracepointDiscovery()
    print(f"\n发现 {len(tp.discover()[:50])} 个 tracepoint（前50）")
