# SPDX-License-Identifier: MIT
"""
ai_linux/config.py — 配置管理
"""

import os
import json
import yaml
from pathlib import Path
from typing import Optional, Dict, Any, List
from dataclasses import dataclass, field, asdict
from copy import deepcopy


@dataclass
class BackendConfig:
    """后端配置"""
    provider: str = "deepseek"
    api_key_env: str = "DEEPSEEK_API_KEY"
    model: str = "deepseek-chat"
    api_timeout_ms: int = 5000
    max_retries: int = 3
    temperature: float = 0.3
    max_tokens: int = 512
    rate_limit_per_min: int = 0

    def to_dict(self) -> Dict:
        return asdict(self)


@dataclass
class RoutingConfig:
    """路由配置"""
    mode: str = "async"  # sync / async / override
    sync_timeout_ms: int = 1000
    sched_threshold: int = 7000
    io_threshold: int = 7000
    sec_threshold: int = 8000
    mem_threshold: int = 6000
    fallback: str = "layer1"

    def to_dict(self) -> Dict:
        return asdict(self)


@dataclass
class SkillWeight:
    """Skill 权重"""
    skill: str
    deepseek_weight: int
    kimi_weight: int


@dataclass
class MCPConfig:
    """MCP 配置"""
    enabled: bool = True
    tool_timeout_ms: int = 3000
    tools: List[str] = field(default_factory=list)

    def to_dict(self) -> Dict:
        return asdict(self)


@dataclass
class NetworkConfig:
    """网络配置"""
    listen_host: str = "127.0.0.1"
    listen_port: int = 9999
    use_tcp: bool = True
    unix_socket: str = "/var/run/ai-layer2.sock"
    ws_port: int = 9998

    def to_dict(self) -> Dict:
        return asdict(self)


@dataclass
class AIConfig:
    """
    AI Linux 完整配置

    用法：
        # 从文件加载
        cfg = AIConfig.load("~/.ai-linux/config.yaml")

        # 从环境变量加载
        cfg = AIConfig.from_env()

        # 修改配置
        cfg.backend.api_key_env = "MY_KEY"
        cfg.routing.mode = "override"

        # 保存
        cfg.save("~/.ai-linux/config.yaml")

        # 使用
        ai = AIClient(config=cfg)
    """

    version: str = "1.0"
    backend: BackendConfig = field(default_factory=BackendConfig)
    routing: RoutingConfig = field(default_factory=RoutingConfig)
    network: NetworkConfig = field(default_factory=NetworkConfig)
    mcp: MCPConfig = field(default_factory=MCPConfig)

    # Skill 权重矩阵
    skill_weights: List[Dict] = field(default_factory=list)

    # 日志
    log_level: str = "info"
    log_file: str = "/var/log/ai-linux.log"

    # 缓存
    cache_enabled: bool = True
    cache_ttl_ms: int = 5000

    def __post_init__(self):
        # 默认 Skill 权重
        if not self.skill_weights:
            self.skill_weights = [
                {"skill": "scheduling",   "deepseek": 70, "kimi": 30},
                {"skill": "io_network",   "deepseek": 50, "kimi": 50},
                {"skill": "security",      "deepseek": 40, "kimi": 60},
                {"skill": "memory",       "deepseek": 65, "kimi": 35},
                {"skill": "code",          "deepseek": 80, "kimi": 20},
                {"skill": "orchestrate",  "deepseek": 30, "kimi": 70},
            ]

    def to_dict(self) -> Dict:
        return {
            "version": self.version,
            "backend": self.backend.to_dict(),
            "routing": self.routing.to_dict(),
            "network": self.network.to_dict(),
            "mcp": self.mcp.to_dict(),
            "skill_weights": self.skill_weights,
            "log_level": self.log_level,
            "log_file": self.log_file,
            "cache_enabled": self.cache_enabled,
            "cache_ttl_ms": self.cache_ttl_ms,
        }

    @classmethod
    def from_dict(cls, d: Dict) -> "AIConfig":
        """从字典创建配置"""
        backend = BackendConfig(**d.get("backend", {}))
        routing = RoutingConfig(**d.get("routing", {}))
        network = NetworkConfig(**d.get("network", {}))
        mcp = MCPConfig(**d.get("mcp", {}))

        return cls(
            version=d.get("version", "1.0"),
            backend=backend,
            routing=routing,
            network=network,
            mcp=mcp,
            skill_weights=d.get("skill_weights", []),
            log_level=d.get("log_level", "info"),
            log_file=d.get("log_file", "/var/log/ai-linux.log"),
            cache_enabled=d.get("cache_enabled", True),
            cache_ttl_ms=d.get("cache_ttl_ms", 5000),
        )

    @classmethod
    def load(cls, path: str) -> "AIConfig":
        """
        从文件加载配置

        支持 YAML 和 JSON 格式。
        """
        p = Path(path).expanduser()

        if not p.exists():
            # 返回默认配置
            return cls()

        with open(p) as f:
            if p.suffix in (".yaml", ".yml"):
                d = yaml.safe_load(f)
            else:
                d = json.load(f)

        return cls.from_dict(d)

    @classmethod
    def from_env(cls) -> "AIConfig":
        """
        从环境变量加载配置

        环境变量：
            AI_BACKEND=deepseek|kimi
            AI_MODEL=deepseek-chat|moonshot-v1-8k
            AI_MODE=sync|async|override
            AI_THRESHOLD_SCHED=7000
            AI_THRESHOLD_IO=7000
            AI_THRESHOLD_SEC=8000
            DEEPSEEK_API_KEY=sk-xxx
            KIMI_API_KEY=sk-xxx
            AI_LOG_LEVEL=info|debug
        """
        cfg = cls()

        backend = cfg.backend
        if os.getenv("AI_BACKEND"):
            backend.provider = os.getenv("AI_BACKEND")

        if os.getenv("AI_MODEL"):
            backend.model = os.getenv("AI_MODEL")

        if os.getenv("DEEPSEEK_API_KEY"):
            backend.api_key_env = "DEEPSEEK_API_KEY"
        elif os.getenv("KIMI_API_KEY"):
            backend.api_key_env = "KIMI_API_KEY"

        routing = cfg.routing
        if os.getenv("AI_MODE"):
            routing.mode = os.getenv("AI_MODE")

        if os.getenv("AI_THRESHOLD_SCHED"):
            routing.sched_threshold = int(os.getenv("AI_THRESHOLD_SCHED"))

        if os.getenv("AI_THRESHOLD_IO"):
            routing.io_threshold = int(os.getenv("AI_THRESHOLD_IO"))

        if os.getenv("AI_THRESHOLD_SEC"):
            routing.sec_threshold = int(os.getenv("AI_THRESHOLD_SEC"))

        if os.getenv("AI_LOG_LEVEL"):
            cfg.log_level = os.getenv("AI_LOG_LEVEL")

        return cfg

    def save(self, path: str, format: str = "yaml"):
        """
        保存配置到文件

        参数：
            path: 文件路径
            format: yaml 或 json
        """
        p = Path(path).expanduser()
        p.parent.mkdir(parents=True, exist_ok=True)

        d = self.to_dict()

        with open(p, "w") as f:
            if format == "json":
                json.dump(d, f, indent=2, ensure_ascii=False)
            else:
                yaml.dump(d, f, default_flow_style=False, allow_unicode=True,
                         sort_keys=False)

    def merge(self, other: "AIConfig") -> "AIConfig":
        """
        合并另一个配置（other 优先级更高）

        用于分层配置：默认配置 + 用户配置 + 环境变量
        """
        result = deepcopy(self)

        if other.backend.provider:
            result.backend.provider = other.backend.provider
        if other.backend.api_key_env:
            result.backend.api_key_env = other.backend.api_key_env
        if other.backend.model:
            result.backend.model = other.backend.model
        if other.backend.api_timeout_ms:
            result.backend.api_timeout_ms = other.backend.api_timeout_ms

        if other.routing.mode:
            result.routing.mode = other.routing.mode
        result.routing.sched_threshold = other.routing.sched_threshold
        result.routing.io_threshold = other.routing.io_threshold
        result.routing.sec_threshold = other.routing.sec_threshold

        if other.network.listen_port:
            result.network.listen_port = other.network.listen_port

        if other.skill_weights:
            result.skill_weights = other.skill_weights

        result.log_level = other.log_level
        result.cache_enabled = other.cache_enabled

        return result

    def get_api_key(self) -> Optional[str]:
        """获取实际 API 密钥"""
        env_var = self.backend.api_key_env
        if env_var:
            return os.getenv(env_var)
        return None

    def get_skill_weight(self, skill: str) -> Dict[str, int]:
        """获取指定 Skill 的权重"""
        for w in self.skill_weights:
            if w.get("skill") == skill:
                return {
                    "deepseek": w.get("deepseek", 50),
                    "kimi": w.get("kimi", 50),
                }
        return {"deepseek": 50, "kimi": 50}

    def select_backend(self, skill: str) -> str:
        """根据 Skill 选择最优 Backend"""
        w = self.get_skill_weight(skill)
        if w["deepseek"] >= w["kimi"]:
            return "deepseek"
        return "kimi"

    def __repr__(self) -> str:
        return (
            f"AIConfig(\n"
            f"  backend={self.backend.provider},\n"
            f"  model={self.backend.model},\n"
            f"  routing.mode={self.routing.mode},\n"
            f"  network.port={self.network.listen_port}\n"
            f")"
        )


# 全局默认配置
_default_config: Optional[AIConfig] = None


def get_default_config() -> AIConfig:
    """获取全局默认配置（单例）"""
    global _default_config
    if _default_config is None:
        # 尝试从多个位置加载
        paths = [
            Path("~/.ai-linux/config.yaml").expanduser(),
            Path("~/.config/ai-linux/config.yaml").expanduser(),
            Path("/etc/ai-linux/config.yaml"),
        ]

        for p in paths:
            if p.exists():
                _default_config = AIConfig.load(str(p))
                break

        if _default_config is None:
            # 从环境变量加载
            _default_config = AIConfig.from_env()

    return _default_config


def set_default_config(cfg: AIConfig):
    """设置全局默认配置"""
    global _default_config
    _default_config = cfg
