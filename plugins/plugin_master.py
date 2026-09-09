#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
plugin_master.py — KAI Linux 全插件热插拔主控
====================================================================
一切模块和功能皆插件，支持运行时热插拔。

统一 11 类插件：
  kernel / backend / skill / mcptool / feature /
  collector / notifier / gwapi / ui / hwaccel / sec

统一生命周期：
  REGISTERED → LOADED → ENABLED → RUNNING → DISABLED → UNLOADED

事件总线：插件状态迁移广播，联动（后端下线→LB摘除 等）。

用法：
    from plugin_master import master

    master.register(BackendPlugin("deepseek", ...))
    master.register(KernelModule("kai_infer", module_path="kai_infer"))
    master.enable_all()
    master.hot_plug()
"""

import os
import sys
import time
import json
import uuid
import signal
import shutil
import inspect
import logging
import threading
import importlib
import importlib.util
import subprocess
from typing import Dict, List, Optional, Any, Callable, Set, Tuple
from dataclasses import dataclass, field
from enum import Enum

log = logging.getLogger("kai-plugin")

# ============================================================================
# 插件种类（Everything-as-a-Plugin）
# ============================================================================

class PluginKind(str, Enum):
    KERNEL    = "kernel"     # 内核模块
    BACKEND   = "backend"    # 推理后端（DeepSeek/Kimi/本地/集成）
    SKILL     = "skill"      # 能力（调度/IO/安全/内存/代码/编排）
    MCPTOOL   = "mcptool"    # MCP 工具
    FEATURE   = "feature"    # 功能引擎（缓存/负载均衡/会话…）
    COLLECTOR = "collector"  # 数据采集（cpu/mem/net/proc）
    NOTIFIER  = "notifier"   # 通知渠道（webhook/钉钉/slack/云）
    GWAPI     = "gwapi"      # 网关 API（rest/ws/netlink）
    UI        = "ui"         # 界面（cli/tui/web/monitor）
    HWACCEL   = "hwaccel"    # 硬件后端（nvidia/amd/intel/ascend）
    SEC       = "sec"        # 安全（crypto/tls/rbac）


# ============================================================================
# 插件状态
# ============================================================================

class PluginState(str, Enum):
    REGISTERED = "registered"
    LOADED     = "loaded"
    ENABLED    = "enabled"
    RUNNING    = "running"
    DEGRADED   = "degraded"
    DISABLED   = "disabled"
    UNLOADED   = "unloaded"
    ERROR      = "error"


# ============================================================================
# 插件基类
# ============================================================================

@dataclass
class Plugin:
    """一切皆插件的统一基类"""

    name: str
    kind: PluginKind
    version: str = "1.0.0"
    description: str = ""
    depends: List[str] = field(default_factory=list)   # 依赖插件名
    provides: List[str] = field(default_factory=list)  # 提供的能力名
    autoload: bool = True
    priority: int = 100
    config: Dict[str, Any] = field(default_factory=dict)

    # --- 运行时状态 ---
    state: PluginState = PluginState.REGISTERED
    error: str = ""
    started_at: float = 0.0
    health: int = 100            # 0-100
    pid: int = 0                 # 有子进程时填充
    meta: Dict[str, Any] = field(default_factory=dict)

    # 内部
    _master: Any = None

    # ================= 生命周期钩子（子类覆写） =================

    def on_load(self):
        """加载：分配资源、导入模块"""
        return True

    def on_enable(self):
        """启用：注册到系统、打开端口"""
        return True

    def on_start(self):
        """启动：拉起线程/进程"""
        return True

    def on_stop(self):
        return True

    def on_disable(self):
        return True

    def on_unload(self):
        return True

    def on_reload(self):
        """热重载"""
        self.on_stop(); self.on_disable()
        self.on_load(); self.on_enable(); self.on_start()
        return True

    def health_check(self) -> int:
        """返回 0-100 健康分"""
        return self.health

    # ================= 主控触发 =================

    def _set_state(self, state: PluginState, error: str = ""):
        old = self.state
        self.state = state
        self.error = error
        if self._master:
            self._master._emit(self, old, state)

    def load(self):
        if self.state in (PluginState.LOADED, PluginState.ENABLED,
                          PluginState.RUNNING):
            return
        try:
            if self.on_load():
                self._set_state(PluginState.LOADED)
        except Exception as e:
            self._set_state(PluginState.ERROR, str(e))
            log.error("插件 %s load 失败: %s", self.name, e)

    def enable(self):
        if self.state == PluginState.RUNNING:
            return
        self.load() if self.state == PluginState.REGISTERED else None
        if self.state != PluginState.LOADED:
            return
        try:
            if self.on_enable():
                self._set_state(PluginState.ENABLED)
        except Exception as e:
            self._set_state(PluginState.ERROR, str(e))
            log.error("插件 %s enable 失败: %s", self.name, e)

    def start(self):
        if self.state == PluginState.RUNNING:
            return
        self.enable() if self.state in (PluginState.LOADED,) else None
        if self.state not in (PluginState.ENABLED, PluginState.DISABLED):
            pass
        try:
            if self.on_start():
                self.started_at = time.time()
                self._set_state(PluginState.RUNNING)
        except Exception as e:
            self._set_state(PluginState.ERROR, str(e))

    def stop(self):
        try:
            self.on_stop()
            self._set_state(PluginState.ENABLED)
        except Exception as e:
            log.error("插件 %s stop 失败: %s", self.name, e)

    def disable(self):
        self.stop() if self.state == PluginState.RUNNING else None
        try:
            self.on_disable()
            self._set_state(PluginState.DISABLED)
        except Exception as e:
            log.error("插件 %s disable 失败: %s", self.name, e)

    def unload(self):
        if self.state in (PluginState.RUNNING, PluginState.ENABLED):
            self.disable()
        try:
            self.on_unload()
            self._set_state(PluginState.UNLOADED)
        except Exception as e:
            log.error("插件 %s unload 失败: %s", self.name, e)

    # ================= 便利 =================

    def snapshot(self) -> Dict:
        return {
            "name": self.name,
            "kind": self.kind.value,
            "version": self.version,
            "description": self.description,
            "state": self.state.value,
            "health": self.health,
            "depends": self.depends,
            "error": self.error,
        }


# ============================================================================
# 各类型通用插件实现
# ============================================================================

class KernelModule(Plugin):
    """内核模块插件：insmod/modprobe/rmmod 热插拔"""

    def __init__(self, name: str, module_path: Optional[str] = None,
                 params: Optional[Dict] = None, **kw):
        super().__init__(name=name, kind=PluginKind.KERNEL, **kw)
        self.module_path = module_path      # 指向 .ko 的绝对路径
        self.params = params or {}
        self.modname = name

    def _loaded_kernel(self) -> bool:
        try:
            r = subprocess.run(["lsmod"], capture_output=True, text=True)
            return self.modname in r.stdout
        except Exception:
            return False

    def on_load(self):
        if self.module_path and os.path.exists(self.module_path):
            cmd = ["insmod", self.module_path] + \
                  [f"{k}={v}" for k, v in self.params.items()]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                log.warning("insmod %s: %s", self.modname, r.stderr)
        return True

    def on_unload(self):
        r = subprocess.run(["rmmod", self.modname], capture_output=True)
        return r.returncode == 0

    def on_enable(self):
        # 通过 proc 接口启用（如果可用）
        proc = "/proc/ai_plugins/list"
        if os.path.exists(proc):
            with open(proc, "w") as f:
                f.write(f"enable {self.name}")
        return True

    def on_disable(self):
        proc = "/proc/ai_plugins/list"
        if os.path.exists(proc):
            with open(proc, "w") as f:
                f.write(f"disable {self.name}")
        return True

    def health_check(self) -> int:
        return 100 if self._loaded_kernel() else 0


class BackendPlugin(Plugin):
    """推理后端插件"""

    def __init__(self, name: str, infer_fn: Optional[Callable] = None,
                 kind=PluginKind.BACKEND, **kw):
        super().__init__(name=name, kind=kind, **kw)
        self.infer_fn = infer_fn

    def infer(self, prompt: str, **kw) -> Dict:
        if self.state != PluginState.RUNNING and self.state != PluginState.ENABLED:
            return {"error": f"backend {self.name} not running"}
        if self.infer_fn:
            return self.infer_fn(prompt, **kw)
        return {"error": "no infer_fn", "backend": self.name}


class SkillPlugin(Plugin):
    """能力插件：把 skill_registry 的每项变成插件"""

    def __init__(self, name: str, category: str, handler=None, **kw):
        super().__init__(name=name, kind=PluginKind.SKILL, **kw)
        self.category = category
        self.handler = handler

    def handle(self, ctx: Dict) -> Dict:
        if self.handler:
            return self.handler(ctx)
        return {"error": "no handler"}


class CollectorPlugin(Plugin):
    """数据采集器插件"""

    def __init__(self, name: str, collect_fn: Optional[Callable] = None,
                 interval_s: float = 2.0, **kw):
        super().__init__(name=name, kind=PluginKind.COLLECTOR, **kw)
        self.collect_fn = collect_fn
        self.interval_s = interval_s
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self.latest: Any = None

    def on_start(self):
        if self.collect_fn:
            self._stop.clear()
            self._thread = threading.Thread(target=self._loop, daemon=True)
            self._thread.start()
        return True

    def on_stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1)

    def _loop(self):
        while not self._stop.is_set():
            try:
                self.latest = self.collect_fn()
            except Exception as e:
                log.error("collector %s: %s", self.name, e)
            self._stop.wait(self.interval_s)


class NotifierPlugin(Plugin):
    """通知渠道插件"""

    def __init__(self, name: str, notify_fn: Optional[Callable] = None, **kw):
        super().__init__(name=name, kind=PluginKind.NOTIFIER, **kw)
        self.notify_fn = notify_fn

    def notify(self, level: str, title: str, message: str = "") -> bool:
        if self.state not in (PluginState.RUNNING, PluginState.ENABLED):
            return False
        if self.notify_fn:
            return bool(self.notify_fn(level, title, message))
        return False


class ServicePlugin(Plugin):
    """有子进程的服务插件（web/ws/gateway/tui）"""

    def __init__(self, name: str, command: List[str],
                 kind: PluginKind = PluginKind.GWAPI,
                 port: int = 0, **kw):
        super().__init__(name=name, kind=kind, **kw)
        self.command = command
        self.port = port
        self._proc: Optional[subprocess.Popen] = None

    def on_start(self) -> bool:
        try:
            self._proc = subprocess.Popen(
                self.command, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env={**os.environ, **self.config.get("env", {})})
            self.pid = self._proc.pid
            return True
        except Exception as e:
            log.error("启动服务 %s: %s", self.name, e)
            return False

    def on_stop(self):
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=5)
            except Exception:
                self._proc.kill()
        self._proc = None

    def health_check(self) -> int:
        if not self._proc:
            return 0
        if self._proc.poll() is not None:
            return 0  # 进程已死
        if self.port:
            # 检查端口（可选）
            try:
                import socket
                s = socket.create_connection(("127.0.0.1", self.port), 0.5)
                s.close()
                return 100
            except Exception:
                return 50
        return 100


class PythonModulePlugin(Plugin):
    """从 .py 文件加载的通用插件"""

    def __init__(self, name: str, module_path: str, kind: PluginKind = PluginKind.FEATURE,
                 instance_name: Optional[str] = None, **kw):
        super().__init__(name=name, kind=kind, **kw)
        self.module_path = module_path
        self.instance_name = instance_name
        self.module = None

    def on_load(self) -> bool:
        try:
            mod_name = f"kai_plugin_{uuid.uuid4().hex[:8]}"
            spec = importlib.util.spec_from_file_location(mod_name, self.module_path)
            self.module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.module)
            return True
        except Exception as e:
            log.error("加载模块 %s: %s", self.module_path, e)
            return False


# ============================================================================
# 事件总线
# ============================================================================

class EventBus:
    def __init__(self):
        self._subs: Dict[str, List[Callable]] = {}

    def subscribe(self, event: str, cb: Callable):
        self._subs.setdefault(event, []).append(cb)

    def emit(self, event: str, **data):
        for cb in self._subs.get(event, []):
            try:
                cb(**data)
            except Exception as e:
                log.error("事件处理 %s: %s", event, e)


# ============================================================================
# PluginMaster — 全插件主控
# ============================================================================

class PluginMaster:
    """
    全插件热插拔主控

    - 统一注册中心
    - 依赖解析（拓扑排序）
    - 生命周期驱动（自动链式迁移）
    - 事件总线广播
    - 热插拔（plug/unplug）
    - watchdog 健康检查
    """

    def __init__(self, watch: bool = True):
        self.plugins: Dict[str, Plugin] = {}
        self.bus = EventBus()
        self._lock = threading.RLock()
        self._watch = watch
        self._builtin_helpers = {}

    # ------------------------------------------------------------ 注册

    def register(self, plugin: Plugin) -> Plugin:
        with self._lock:
            if plugin.name in self.plugins:
                raise ValueError(f"插件已存在: {plugin.name}")
            plugin._master = self
            self.plugins[plugin.name] = plugin
            plugin._set_state(PluginState.REGISTERED)
            self.bus.emit("plugin.registered", name=plugin.name)
        return plugin

    def unregister(self, name: str) -> bool:
        with self._lock:
            plugin = self.plugins.pop(name, None)
            if plugin:
                plugin.unload()
                self.bus.emit("plugin.unregistered", name=name)
                return True
            return False

    def get(self, name: str) -> Optional[Plugin]:
        return self.plugins.get(name)

    def list(self, kind: Optional[PluginKind] = None) -> List[Plugin]:
        if kind:
            return [p for p in self.plugins.values() if p.kind == kind]
        return list(self.plugins.values())

    # ------------------------------------------------------------ 依赖

    def _deps_of(self, name: str) -> List[str]:
        return self.plugins.get(name, None).depends if name in self.plugins else []

    def _topo_order(self) -> List[str]:
        """拓扑排序：依赖先加载"""
        visited: Set[str] = set()
        order: List[str] = []
        temp: Set[str] = set()

        def dfs(name: str):
            if name in visited:
                return
            if name in temp:
                raise RuntimeError(f"依赖循环: {name}")
            temp.add(name)
            for dep in self._deps_of(name):
                if dep in self.plugins:
                    dfs(dep)
            temp.remove(name)
            visited.add(name)
            order.append(name)

        for name in list(self.plugins):
            dfs(name)
        return order

    # ------------------------------------------------------------ 生命周期链

    def load(self, name: str):
        with self._lock:
            p = self.plugins.get(name)
            if p:
                # 先加载依赖
                for dep in p.depends:
                    if dep in self.plugins and self.plugins[dep].state in (
                            PluginState.REGISTERED, PluginState.UNLOADED):
                        self.load(dep)
                p.load()
        return p

    def enable(self, name: str):
        p = self.plugins.get(name)
        if not p:
            return None
        self.load(name)
        with self._lock:
            p.enable()
        return p

    def start(self, name: str):
        self.enable(name)
        p = self.plugins.get(name)
        if not p:
            return None
        with self._lock:
            p.start()
        return p

    def stop(self, name: str):
        p = self.plugins.get(name)
        if p:
            with self._lock:
                p.stop()

    def disable(self, name: str):
        p = self.plugins.get(name)
        if p:
            with self._lock:
                p.disable()

    def unload(self, name: str):
        p = self.plugins.get(name)
        if p:
            with self._lock:
                p.unload()

    def reload(self, name: str):
        p = self.plugins.get(name)
        if p:
            with self._lock:
                p.on_reload()
            self.bus.emit("plugin.reloaded", name=name)

    # ------------------------------------------------------------ 批量

    def enable_all(self):
        for name in self._topo_order():
            try:
                self.enable(name)
            except Exception as e:
                log.error("enable %s: %s", name, e)

    def start_all(self):
        for name in self._topo_order():
            try:
                self.start(name)
            except Exception as e:
                log.error("start %s: %s", name, e)

    def disable_all(self):
        for name in reversed(self._topo_order()):
            try:
                self.disable(name)
            except Exception:
                pass

    # ------------------------------------------------------------ 热插拔

    def plug(self, plugin: Plugin):
        """热插入：注册 + 自动加载 + 启用 + 启动"""
        self.register(plugin)
        try:
            self.start(plugin.name)
            log.info("🔌 热插入: %s (%s) → %s",
                     plugin.name, plugin.kind.value, plugin.state.value)
        except Exception as e:
            log.error("热插入失败 %s: %s", plugin.name, e)

    def unplug(self, name: str):
        """热拔出：优雅停止 → 卸载 → 注销"""
        with self._lock:
            p = self.plugins.get(name)
            if not p:
                return False
            # 通知依赖它的插件
            for other in self.plugins.values():
                if name in other.depends:
                    log.info("依赖 %s 的插件 %s 将降级", name, other.name)
                    other.health = 0
                    other._set_state(PluginState.DEGRADED)
            p.unload()
            self.plugins.pop(name, None)
            self.bus.emit("plugin.unplugged", name=name)
            log.info("🔌 热拔出: %s", name)
            return True

    # ------------------------------------------------------------ watchdog

    def watchdog_tick(self):
        for p in list(self.plugins.values()):
            if p.state != PluginState.RUNNING:
                continue
            try:
                h = p.health_check()
                p.health = h
                if h == 0:
                    log.warning("插件 %s 失活，尝试重启", p.name)
                    p.on_stop()
                    p.on_start()
                    p._set_state(PluginState.RUNNING)
                elif h < 60 and p.state != PluginState.DEGRADED:
                    p._set_state(PluginState.DEGRADED)
                elif h >= 60 and p.state == PluginState.DEGRADED:
                    p._set_state(PluginState.RUNNING)
            except Exception:
                pass

    def run_watchdog(self, interval_s: float = 5.0):
        while True:
            try:
                self.watchdog_tick()
            except Exception as e:
                log.error("watchdog: %s", e)
            time.sleep(interval_s)

    # ------------------------------------------------------------ 内部

    def _emit(self, plugin: Plugin, old: PluginState, new: PluginState):
        if old != new:
            self.bus.emit("plugin.state", name=plugin.name,
                          old=old.value, new=new.value)

    # ------------------------------------------------------------ 快照

    def snapshot(self) -> Dict:
        return {
            "time": time.time(),
            "total": len(self.plugins),
            "plugins": [p.snapshot() for p in self.plugins.values()],
        }

    def export(self, path: str):
        with open(path, "w") as f:
            json.dump(self.snapshot(), f, indent=2, ensure_ascii=False)

    def summary(self) -> str:
        lines = ["", "=" * 78,
                 f"  KAI 全插件热插拔 · 共 {len(self.plugins)} 个插件",
                 "=" * 78]
        by_kind: Dict[str, int] = {}
        for p in self.plugins.values():
            by_kind[p.kind.value] = by_kind.get(p.kind.value, 0) + 1

        lines.append("  分类: " + "  ".join(
            f"{k}={v}" for k, v in sorted(by_kind.items())))
        lines.append("")
        lines.append(f"{'插件名':<24}{'类型':<12}{'状态':<12}{'健康':>5}  描述")
        lines.append("-" * 78)
        for p in sorted(self.plugins.values(), key=lambda x: x.kind.value):
            mark = {"running": "●", "enabled": "◐", "disabled": "○",
                    "error": "✗", "degraded": "!"}.get(p.state.value, "·")
            lines.append(
                f"{mark} {p.name:<22}{p.kind.value:<12}{p.state.value:<12}"
                f"{p.health:>4}   {p.description[:30]}")
        lines.append("=" * 78)
        return "\n".join(lines)


# ============================================================================
# 全局单例
# ============================================================================

master = PluginMaster()


# ============================================================================
# 内置注册表 —— 把现有 KAI Linux 模块注册为插件
# ============================================================================

def register_builtin_kernel_plugins(m: PluginMaster, ko_dir: Optional[str] = None):
    """注册内核模块插件（真实 .ko 存在时才 insmod）"""
    ko_dir = ko_dir or os.environ.get("KAI_KO_DIR", "")
    modules = [
        # name, .ko 文件名, 描述
        ("ai_core", "ai_core.ko", "内核 AI Core（推理/模型）"),
        ("kai_cache", "kai_cache.ko", "多级缓存引擎"),
        ("kai_infer", "kai_infer.ko", "量化推理引擎"),
        ("kai_resilience", "kai_resilience.ko", "熔断/超时/降级"),
        ("kai_metrics", "kai_metrics.ko", "Prometheus 指标"),
        ("kai_net_opt", "kai_net_opt.ko", "DNS缓存/连接池"),
        ("kai_mem_opt", "kai_mem_opt.ko", "页面预取/THP/LSM缓存"),
        ("kai_tracing", "kai_tracing.ko", "分布式追踪"),
        ("hello_ai", "hello_ai.ko", "示例模块"),
    ]
    for name, ko, desc in modules:
        path = os.path.join(ko_dir, ko) if ko_dir else ko
        m.register(KernelModule(
            name=name, module_path=path if os.path.exists(path) else None,
            description=desc,
            # 依赖顺序
            depends=["kai_cache"] if name == "kai_infer" else [],
        ))


def register_builtin_backends(m: PluginMaster):
    """注册推理后端插件"""
    # DeepSeek / Kimi 需要 API key
    deepseek_key = os.environ.get("DEEPSEEK_API_KEY", "")
    kimi_key = os.environ.get("KIMI_API_KEY", "")

    def make_http_infer(base, path, key, model):
        def infer(prompt, **kw):
            if not key:
                return {"error": "no api key"}
            import urllib.request
            body = json.dumps({
                "model": model,
                "messages": [{"role": "user", "content": prompt}],
                "temperature": 0.3, "max_tokens": 512,
            }).encode()
            req = urllib.request.Request(
                f"{base}{path}", data=body, headers={
                    "Content-Type": "application/json",
                    "Authorization": f"Bearer {key}"})
            with urllib.request.urlopen(req, timeout=30) as r:
                data = json.loads(r.read())
                return {"backend": model,
                        "text": data["choices"][0]["message"]["content"]}
        return infer

    m.register(BackendPlugin(
        "deepseek",
        infer_fn=make_http_infer("https://api.deepseek.com",
                                 "/v1/chat/completions",
                                 deepseek_key, "deepseek-chat"),
        description="DeepSeek 后端",
    ))
    m.register(BackendPlugin(
        "kimi",
        infer_fn=make_http_infer("https://api.moonshot.cn",
                                 "/v1/chat/completions",
                                 kimi_key, "moonshot-v1-8k"),
        description="Kimi K3 后端",
    ))
    m.register(BackendPlugin(
        "local", infer_fn=lambda p, **k: {"backend": "local",
                                            "text": f"(本地规则) {p[:50]}"},
        description="本地规则引擎（兜底）",
    ))


def register_builtin_collectors(m: PluginMaster):
    """注册系统数据采集器插件"""

    def cpu_collect():
        try:
            with open("/proc/stat") as f:
                parts = f.readline().split()
            vals = [int(x) for x in parts[1:5]]
            total = sum(vals) or 1
            return {"cpu_usage": 100 - vals[3] * 100 // total,
                    "user": vals[0], "system": vals[1]}
        except Exception:
            return {"cpu_usage": 0}

    def mem_collect():
        try:
            mem = {}
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemTotal:"): mem["total"] = int(line.split()[1])
                    if line.startswith("MemAvailable:"): mem["available"] = int(line.split()[1])
            mem["mem_usage"] = (mem.get("total", 1) - mem.get("available", 0)) * 100 // max(mem.get("total", 1), 1)
            return mem
        except Exception:
            return {"mem_usage": 0}

    def net_collect():
        try:
            rx = tx = 0
            with open("/proc/net/dev") as f:
                for line in f:
                    if ":" not in line or " lo:" in line:
                        continue
                    parts = line.split(":")
                    data = parts[1].split()
                    rx += int(data[0]); tx += int(data[8])
            return {"rx": rx, "tx": tx}
        except Exception:
            return {"rx": 0, "tx": 0}

    m.register(CollectorPlugin("cpu", cpu_collect, 2.0,
                               description="CPU 数据采集"))
    m.register(CollectorPlugin("mem", mem_collect, 2.0,
                               description="内存数据采集"))
    m.register(CollectorPlugin("net", net_collect, 5.0,
                               description="网络数据采集"))


def register_builtin_services(m: PluginMaster, base_dir: str = "."):
    """注册网关 API 与 UI 服务插件（可热插拔启动/停止）"""
    build = os.path.join(base_dir, "ui", "build")
    services = [
        # name, command, kind, port, 描述
        ("web", ["kai-web", "--port", "8080"], PluginKind.GWAPI, 8080, "Web Dashboard"),
        ("monitor", ["kai-monitor", "--port", "9090"], PluginKind.GWAPI, 9090, "性能监控"),
        ("ws", [sys.executable, os.path.join(base_dir, "sdk/python/ai_ws_server.py"), "--port", "9998"],
         PluginKind.GWAPI, 9998, "WebSocket 推送"),
        ("tui", ["kai-tui"], PluginKind.UI, 0, "TUI 交互界面"),
    ]
    # 优先用绝对路径
    import shutil as _sh
    for name, cmd, kind, port, desc in services:
        cmd0 = cmd[0]
        if _sh.which(cmd0):
            full = [cmd0] + cmd[1:]
        elif os.path.exists(os.path.join(build, cmd0)):
            full = [os.path.join(build, cmd0)] + cmd[1:]
        else:
            continue
        m.register(ServicePlugin(name=name, command=full,
                                 kind=kind, port=port,
                                 description=desc,
                                 autoload=False))  # 默认不自动启


def register_builtin_features(m: PluginMaster, base_dir: str = "."):
    """把 kai/features/ 功能引擎全部注册为 FEATURE 插件"""
    feat_dir = os.path.join(base_dir, "kai", "features")
    feats = {
        "cache_engine":     ("kai_cache_engine", "多级缓存引擎"),
        "multi_model":      ("multi_model", "多模型集成推理"),
        "load_balancer":    ("load_balancer", "多 API 负载均衡"),
        "session_mgr":      ("session", "会话上下文"),
        "prompt_engine":    ("prompt_engine", "提示词模板引擎"),
        "persistence":      ("persistence", "决策持久化"),
        "tiered_sched":     ("tiered_sched", "分级调度"),
        "tracing":          ("tracing", "分布式追踪"),
        "config_mgr":       ("config_manager", "配置管理"),
        "model_crypto":     ("model_crypto", "模型加密/签名"),
        "tls_mtls":         ("tls_gateway", "TLS/mTLS"),
        "rbac_keys":        ("security", "RBAC/密钥轮换"),
        "plugin_sandbox":   ("plugin_enhance", "插件沙箱/热部署/版本"),
        "hw_backends":      ("backends", "硬件后端探测"),
        "cloud_integration":("cloud_integration", "云平台集成"),
    }
    for key, (mod, desc) in feats.items():
        path = os.path.join(feat_dir, mod + ".py")
        if not os.path.exists(path):
            # 允许从 hw/cloud/security 目录找
            for sub in ("hw", "cloud", "security"):
                alt = os.path.join(base_dir, "kai", sub, mod + ".py")
                if os.path.exists(alt):
                    path = alt
                    break
        if os.path.exists(path):
            m.register(PythonModulePlugin(
                name=key, module_path=path, kind=PluginKind.FEATURE,
                description=desc, autoload=False))


def register_builtin_notifiers(m: PluginMaster):
    """注册通知渠道插件"""
    def make_webhook(url):
        def notify(level, title, message):
            import urllib.request
            body = json.dumps({"level": level, "title": title,
                               "message": message}).encode()
            try:
                req = urllib.request.Request(url, data=body,
                                             headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=5) as r:
                    return r.status == 200
            except Exception:
                return False
        return notify

    def make_stdout():
        def notify(level, title, message):
            print(f"[notify:{level}] {title} — {message}")
            return True
        return notify

    m.register(NotifierPlugin("stdout", make_stdout(),
                              description="标准输出通知", autoload=False))
    m.register(NotifierPlugin("webhook", make_webhook(
        os.environ.get("KAI_WEBHOOK_URL", "http://localhost:9999/alert")),
        description="Webhook 通知", autoload=False))


def register_builtin_skills(m: PluginMaster):
    """注册 Skill 能力插件"""
    skills = [
        ("skill_sched", "调度分析能力"),
        ("skill_io", "网络 IO 分析"),
        ("skill_sec", "安全检测能力"),
        ("skill_mem", "内存分析能力"),
        ("skill_code", "代码生成能力"),
        ("skill_orch", "系统编排能力"),
    ]
    for name, desc in skills:
        m.register(SkillPlugin(name=name, category=name.replace("skill_", ""),
                               description=desc, autoload=False))


def bootstrap() -> PluginMaster:
    """一键初始化全插件系统"""
    m = master
    register_builtin_kernel_plugins(m)
    register_builtin_backends(m)
    register_builtin_collectors(m)
    register_builtin_services(m)
    register_builtin_features(m)
    register_builtin_notifiers(m)
    register_builtin_skills(m)
    return m


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    bootstrap()
    # 默认启用：collectors + backends（有 key 的）+ local
    for name in ["cpu", "mem", "net"]:
        m.enable(name)
    for name in ["deepseek", "kimi", "local"]:
        if name in m.plugins:
            m.enable(name)
    print(m.summary())
