# SPDX-License-Identifier: MIT
"""
ai_linux/models.py — 数据模型
"""

from dataclasses import dataclass, field
from typing import Dict, Any, Optional, List
from enum import Enum


class MessageRole(str, Enum):
    USER = "user"
    AI = "ai"
    SYSTEM = "system"
    TOOL = "tool"


@dataclass
class ChatMessage:
    role: str
    content: str
    timestamp: float = 0.0
    backend: Optional[str] = None
    confidence: Optional[int] = None
    metadata: Dict[str, Any] = field(default_factory=dict)


@dataclass
class AIResponse:
    text: str
    backend: str
    confidence: int
    latency_ms: float
    model: str
    request_id: str = ""
    metadata: Dict[str, Any] = field(default_factory=dict)

    def __str__(self) -> str:
        return self.text

    @property
    def is_error(self) -> bool:
        return "错误" in self.text or "error" in self.text.lower()


@dataclass
class SystemStatus:
    cpu_usage: int = 0
    mem_usage: int = 0
    loadavg: str = "0.00"
    uptime: str = ""
    net_rx: str = ""
    net_tx: str = ""
    timestamp: float = 0.0
    ai_inferences: int = 0
    ai_errors: int = 0
    layer2_connected: bool = False
    backend: str = ""

    @property
    def health_score(self) -> int:
        """健康评分 0-100"""
        score = 100
        score -= min(self.cpu_usage // 2, 30)
        score -= min(self.mem_usage // 3, 30)
        if self.ai_errors > 10:
            score -= 10
        return max(0, score)


@dataclass
class SchedDecision:
    action: str  # promote / demote / migrate / batch / keep
    confidence: int
    target_cpu: Optional[int] = None
    reason: str = ""
    override: bool = False

    @property
    def action_emoji(self) -> str:
        return {
            "promote": "⬆",
            "demote": "⬇",
            "migrate": "↔",
            "batch": "📦",
            "keep": "➡",
            "idle": "💤",
        }.get(self.action, "?")

    def __str__(self) -> str:
        parts = [f"{self.action_emoji} {self.action.upper()}"]
        if self.target_cpu is not None:
            parts.append(f"→ CPU{self.target_cpu}")
        if self.reason:
            parts.append(f"({self.reason[:30]})")
        parts.append(f"[{self.confidence}%]")
        return " ".join(parts)


@dataclass
class SecurityAlert:
    level: str  # low / medium / high / critical
    threat_type: str
    pid: Optional[int] = None
    description: str = ""
    action: str = "allow"  # allow / block / alert
    confidence: int = 0

    @property
    def level_emoji(self) -> str:
        return {
            "low": "🟢",
            "medium": "🟡",
            "high": "🟠",
            "critical": "🔴",
        }.get(self.level, "⚪")


@dataclass
class InferenceRequest:
    prompt: str
    domain: str
    request_id: str = ""
    timeout_ms: int = 5000
    priority: int = 1
    metadata: Dict[str, Any] = field(default_factory=dict)


@dataclass
class InferenceResult:
    request_id: str
    response: AIResponse
    decision: Optional[SchedDecision] = None
    latency_ms: float = 0.0
    cached: bool = False
