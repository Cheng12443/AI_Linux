#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
AI Linux Plugin SDK (Python)

插件管理器，支持：
  - 动态加载 / 卸载插件
  - 插件热更新
  - 插件通信（插件间消息）
  - 插件生命周期管理
  - 与内核插件系统（ai_plugin_mgr.ko）互通

用法：
    from ai_plugin_sdk import PluginManager, Plugin

    pm = PluginManager()
    pm.load_plugin("plugins/sched_plugin.py")
    pm.enable_plugin("sched_plugin")
    result = pm.dispatch("scheduling", {"pid": 1234})
"""

import os
import sys
import json
import time
import hashlib
import importlib.util
import inspect
import logging
import threading
from typing import Dict, Any, List, Optional, Callable
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

logger = logging.getLogger(__name__)


# ============================================================================
# 插件类型
# ============================================================================

class PluginType(str, Enum):
    SCHED       = "scheduling"
    IO          = "io_network"
    SECURITY    = "security"
    MEMORY      = "memory"
    BACKEND     = "backend"
    FILTER      = "filter"
    ORCHESTRATOR = "orchestrator"


class PluginState(str, Enum):
    UNLOADED  = "unloaded"
    LOADED    = "loaded"
    ENABLED   = "enabled"
    DISABLED  = "disabled"
    ERROR     = "error"


# ============================================================================
# 插件基类
# ============================================================================

class Plugin:
    """
    插件基类。所有插件必须继承此类。

    用法：
        class MyPlugin(Plugin):
            name = "my_plugin"
            type = PluginType.SCHED

            def on_load(self):
                pass

            def on_infer(self, ctx):
                return {"decision": "promote", "confidence": 80}
    """

    # 插件元数据（子类必须设置）
    name: str = ""
    version: str = "1.0.0"
    description: str = ""
    type: PluginType = PluginType.BACKEND
    dependencies: List[str] = []

    def __init__(self):
        self._state = PluginState.UNLOADED
        self._config: Dict[str, Any] = {}
        self._stats: Dict[str, int] = {
            "inferences": 0,
            "errors": 0,
            "total_latency_ms": 0,
        }
        self._lock = threading.Lock()

    # --- 生命周期钩子（子类覆盖）---

    def on_load(self):
        """插件加载时调用"""
        pass

    def on_unload(self):
        """插件卸载时调用"""
        pass

    def on_enable(self):
        """插件启用时调用"""
        pass

    def on_disable(self):
        """插件禁用时调用"""
        pass

    # --- 核心接口（子类实现）---

    def on_infer(self, ctx: Dict[str, Any]) -> Dict[str, Any]:
        """
        推理入口

        ctx 包含：
            - input: 输入数据
            - domain: 决策域
            - features: 特征数据
            - timestamp: 时间戳

        返回：
            {"decision": ..., "confidence": ..., "reason": ...}
        """
        raise NotImplementedError

    def on_event(self, event_type: str, data: Dict):
        """事件通知"""
        pass

    # --- 配置（子类可选覆盖）---

    def on_config_change(self, key: str, value: Any):
        """配置变更时调用"""
        pass

    # --- 内部方法（子类不应覆盖）---

    def load(self):
        """加载插件"""
        self._state = PluginState.LOADED
        self.on_load()

    def unload(self):
        """卸载插件"""
        self.on_unload()
        self._state = PluginState.UNLOADED

    def enable(self):
        """启用插件"""
        self.on_enable()
        self._state = PluginState.ENABLED

    def disable(self):
        """禁用插件"""
        self.on_disable()
        self._state = PluginState.DISABLED

    def infer(self, ctx: Dict[str, Any]) -> Dict[str, Any]:
        """推理入口（包装器，添加统计）"""
        if self._state != PluginState.ENABLED:
            return {"error": "plugin not enabled"}

        start = time.time()
        try:
            result = self.on_infer(ctx)
            self._stats["inferences"] += 1
        except Exception as e:
            self._stats["errors"] += 1
            result = {"error": str(e)}

        elapsed = (time.time() - start) * 1000
        self._stats["total_latency_ms"] += elapsed

        return result

    def set_config(self, key: str, value: Any):
        """设置配置"""
        self._config[key] = value
        self.on_config_change(key, value)

    def get_config(self, key: str, default: Any = None) -> Any:
        """获取配置"""
        return self._config.get(key, default)

    @property
    def state(self) -> PluginState:
        return self._state

    @property
    def stats(self) -> Dict:
        with self._lock:
            return dict(self._stats)


# ============================================================================
# 插件管理器
# ============================================================================

@dataclass
class PluginInfo:
    name: str
    version: str
    description: str
    type: PluginType
    state: PluginState
    path: str = ""
    loaded_at: float = 0.0
    inferences: int = 0
    errors: int = 0

    def to_dict(self) -> Dict:
        d = asdict(self)
        d["type"] = self.type.value
        d["state"] = self.state.value
        return d


class PluginManager:
    """
    AI Linux 插件管理器

    支持：
      - 动态加载 Python 插件文件
      - 启用 / 禁用 / 重载插件
      - 插件间消息通信
      - 插件统计与监控
      - 依赖解析（拓扑排序）

    用法：
        pm = PluginManager()
        pm.load_plugin("plugins/sched_plugin.py")
        pm.enable_plugin("sched_plugin")
        result = pm.dispatch("scheduling", {"pid": 1234})
    """

    def __init__(self, plugin_dir: Optional[str] = None):
        self._plugins: Dict[str, Plugin] = {}
        self._info: Dict[str, PluginInfo] = {}
        self._lock = threading.RLock()
        self._plugin_dir = plugin_dir or str(
            Path(__file__).parent / "installed"
        )
        self._plugin_dir.mkdir(parents=True, exist_ok=True)
        self._event_handlers: Dict[str, List[Callable]] = {}

        logger.info(f"PluginManager initialized, dir={self._plugin_dir}")

    # ------------------------------------------------------------------------
    # 插件加载
    # ------------------------------------------------------------------------

    def load_plugin(self, path: str) -> bool:
        """
        从文件加载插件

        path 指向一个 Python 文件，其中包含 Plugin 子类。
        """
        p = Path(path)
        if not p.exists():
            logger.error(f"Plugin file not found: {path}")
            return False

        # 动态导入
        module_name = f"plugin_{hashlib.md5(path.encode()).hexdigest()[:8]}"
        spec = importlib.util.spec_from_file_location(module_name, path)
        if not spec or not spec.loader:
            logger.error(f"Failed to load spec for {path}")
            return False

        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module

        try:
            spec.loader.exec_module(module)
        except Exception as e:
            logger.error(f"Failed to exec plugin {path}: {e}")
            return False

        # 查找 Plugin 子类
        plugin_class = None
        for name, obj in inspect.getmembers(module):
            if (inspect.isclass(obj) and issubclass(obj, Plugin)
                    and obj is not Plugin):
                plugin_class = obj
                break

        if not plugin_class:
            logger.error(f"No Plugin subclass found in {path}")
            return False

        # 实例化
        try:
            instance = plugin_class()
        except Exception as e:
            logger.error(f"Failed to instantiate {path}: {e}")
            return False

        if not instance.name:
            logger.error(f"Plugin in {path} has no name")
            return False

        # 检查依赖
        for dep in instance.dependencies:
            if dep not in self._plugins:
                logger.warning(f"Plugin {instance.name} depends on {dep} "
                               f"(not yet loaded)")

        # 注册
        with self._lock:
            self._plugins[instance.name] = instance
            self._info[instance.name] = PluginInfo(
                name=instance.name,
                version=instance.version,
                description=instance.description,
                type=instance.type,
                state=PluginState.LOADED,
                path=str(p.absolute()),
                loaded_at=time.time(),
            )

        instance.load()
        logger.info(f"Loaded plugin '{instance.name}' from {path}")

        # 发送事件
        self._emit("plugin_loaded", {
            "name": instance.name,
            "type": instance.type.value,
        })

        return True

    def load_all(self):
        """加载插件目录下所有 .py 文件"""
        for path in sorted(self._plugin_dir.glob("*.py")):
            if path.name.startswith("_"):
                continue
            self.load_plugin(str(path))

    # ------------------------------------------------------------------------
    # 插件生命周期
    # ------------------------------------------------------------------------

    def unload_plugin(self, name: str) -> bool:
        """卸载插件"""
        with self._lock:
            if name not in self._plugins:
                return False
            plugin = self._plugins[name]
            info = self._info.get(name)

        plugin.unload()
        del self._plugins[name]

        if info:
            info.state = PluginState.UNLOADED

        logger.info(f"Unloaded plugin '{name}'")
        return True

    def enable_plugin(self, name: str) -> bool:
        """启用插件"""
        with self._lock:
            if name not in self._plugins:
                logger.error(f"Plugin not found: {name}")
                return False
            plugin = self._plugins[name]

        if plugin.state == PluginState.ENABLED:
            return True

        plugin.enable()
        self._info[name].state = PluginState.ENABLED

        self._emit("plugin_enabled", {"name": name})
        logger.info(f"Enabled plugin '{name}'")
        return True

    def disable_plugin(self, name: str) -> bool:
        """禁用插件"""
        with self._lock:
            if name not in self._plugins:
                return False
            plugin = self._plugins[name]

        plugin.disable()
        self._info[name].state = PluginState.DISABLED
        self._emit("plugin_disabled", {"name": name})
        return True

    def reload_plugin(self, name: str) -> bool:
        """重载插件（热更新）"""
        info = self._info.get(name)
        if not info:
            return False

        path = info.path
        self.unload_plugin(name)
        return self.load_plugin(path)

    # ------------------------------------------------------------------------
    # 插件查询
    # ------------------------------------------------------------------------

    def get_plugin(self, name: str) -> Optional[Plugin]:
        """获取插件实例"""
        return self._plugins.get(name)

    def list_plugins(self, type: Optional[PluginType] = None) -> List[PluginInfo]:
        """列出插件"""
        with self._lock:
            result = list(self._info.values())
        if type:
            result = [p for p in result if p.type == type]
        return result

    def get_plugin_names(self, type: Optional[PluginType] = None) -> List[str]:
        """获取插件名称列表"""
        return [p.name for p in self.list_plugins(type)]

    # ------------------------------------------------------------------------
    # 推理分发
    # ------------------------------------------------------------------------

    def dispatch(self, domain: str, ctx: Dict[str, Any]) -> Dict[str, Any]:
        """
        分发推理到匹配的插件

        domain: PluginType 值或字符串
        ctx: 推理上下文

        返回第一个成功插件的结果。
        """
        plugin_type = PluginType(domain) if isinstance(domain, str) else domain

        for plugin in self._plugins.values():
            if plugin.type != plugin_type:
                continue
            if plugin.state != PluginState.ENABLED:
                continue

            result = plugin.infer(ctx)
            if "error" not in result:
                return result

        return {"error": f"No plugin available for domain '{domain}'"}

    def dispatch_all(self, domain: str, ctx: Dict[str, Any]) -> List[Dict]:
        """分发到所有匹配插件，返回所有结果"""
        plugin_type = PluginType(domain) if isinstance(domain, str) else domain
        results = []

        for plugin in self._plugins.values():
            if plugin.type != plugin_type:
                continue
            if plugin.state != PluginState.ENABLED:
                continue

            result = plugin.infer(ctx)
            result["plugin"] = plugin.name
            results.append(result)

        return results

    # ------------------------------------------------------------------------
    # 插件通信
    # ------------------------------------------------------------------------

    def send_to_plugin(self, plugin_name: str,
                       message: Dict[str, Any]) -> bool:
        """向指定插件发送消息"""
        plugin = self._plugins.get(plugin_name)
        if not plugin:
            return False

        plugin.on_event("message", message)
        return True

    def broadcast(self, message: Dict[str, Any]):
        """向所有插件广播消息"""
        for plugin in self._plugins.values():
            plugin.on_event("message", message)

    def on_event(self, event_type: str, callback: Callable):
        """订阅插件事件"""
        if event_type not in self._event_handlers:
            self._event_handlers[event_type] = []
        self._event_handlers[event_type].append(callback)

    def _emit(self, event_type: str, data: Dict):
        """触发事件"""
        for cb in self._event_handlers.get(event_type, []):
            try:
                cb(data)
            except Exception as e:
                logger.error(f"Event handler error: {e}")

    # ------------------------------------------------------------------------
    # 统计
    # ------------------------------------------------------------------------

    def get_stats(self) -> Dict:
        """获取全局统计"""
        total_inf = sum(p.stats["inferences"] for p in self._plugins.values())
        total_err = sum(p.stats["errors"] for p in self._plugins.values())
        total_lat = sum(p.stats["total_latency_ms"] for p in self._plugins.values())

        return {
            "total_plugins": len(self._plugins),
            "enabled_plugins": sum(1 for p in self._plugins.values()
                                  if p.state == PluginState.ENABLED),
            "total_inferences": total_inf,
            "total_errors": total_err,
            "avg_latency_ms": total_lat / max(total_inf, 1),
        }

    def get_plugin_stats(self, name: str) -> Optional[Dict]:
        """获取指定插件的统计"""
        plugin = self._plugins.get(name)
        if not plugin:
            return None
        return plugin.stats


# ============================================================================
# 内置插件示例
# ============================================================================

class SchedulingPlugin(Plugin):
    """调度插件示例"""
    name = "scheduling_plugin"
    description = "调度决策插件"
    type = PluginType.SCHED
    version = "1.0.0"

    def on_infer(self, ctx: Dict[str, Any]) -> Dict[str, Any]:
        features = ctx.get("features", {})
        cpu_util = features.get("cpu_util", 512)
        nivcsw = features.get("nivcsw", 0)
        nvcsw = features.get("nvcsw", 0)

        score = 0
        if cpu_util > 512: score += 30
        if nivcsw > 10: score += 25
        if nvcsw > 50: score += 15

        if score > 70:
            return {"decision": "promote", "confidence": min(score, 100)}
        elif score < 30:
            return {"decision": "demote", "confidence": max(score, 0)}
        else:
            return {"decision": "keep", "confidence": 50}


class SecurityPlugin(Plugin):
    """安全插件示例"""
    name = "security_plugin"
    description = "安全检测插件"
    type = PluginType.SECURITY
    version = "1.0.0"

    def on_infer(self, ctx: Dict[str, Any]) -> Dict[str, Any]:
        comm = ctx.get("comm", "")
        argv = ctx.get("argv", "")

        threats = ["wget", "curl", "nc", "ncat", "/etc/passwd", "/etc/shadow",
                   "LD_PRELOAD", "ptrace", "chmod +s"]

        detected = [t for t in threats if t in argv]

        if detected:
            return {
                "decision": "block",
                "confidence": 90,
                "threat": ",".join(detected),
                "level": "high",
            }
        return {"decision": "allow", "confidence": 80, "level": "low"}


class NetworkPlugin(Plugin):
    """网络插件示例"""
    name = "network_plugin"
    description = "网络 IO 分析插件"
    type = PluginType.IO
    version = "1.0.0"

    def on_infer(self, ctx: Dict[str, Any]) -> Dict[str, Any]:
        dst_ip = ctx.get("dst_ip", "")
        dst_port = ctx.get("dst_port", 0)

        # 简单端口检测
        risky_ports = {22, 23, 3389, 4444, 6667}
        if dst_port in risky_ports:
            return {
                "decision": "alert",
                "confidence": 75,
                "reason": f"dst_port={dst_port} 为高风险端口",
            }

        return {"decision": "pass", "confidence": 85}


# ============================================================================
# 快捷入口
# ============================================================================

def create_default_manager() -> PluginManager:
    """创建默认插件管理器"""
    pm = PluginManager()
    pm._plugins["scheduling_plugin"] = SchedulingPlugin()
    pm._plugins["security_plugin"]   = SecurityPlugin()
    pm._plugins["network_plugin"]    = NetworkPlugin()

    for name, p in pm._plugins.items():
        pm._info[name] = PluginInfo(
            name=p.name,
            version=p.version,
            description=p.description,
            type=p.type,
            state=PluginState.ENABLED,
            loaded_at=time.time(),
        )
        p._state = PluginState.ENABLED

    return pm
