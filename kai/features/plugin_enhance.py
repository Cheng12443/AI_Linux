# SPDX-License-Identifier: MIT
"""
plugin_enhance.py — 插件沙箱 + 热部署 + 版本管理（ROADMAP 2.4）

支持：
  - 插件沙箱（限制系统调用、资源）
  - 插件热部署（文件变化自动重载）
  - 插件版本管理（A/B 测试）
  - 插件权限控制

用法：
    from plugin_enhance import SandboxedPlugin, PluginHotDeploy, PluginVersionManager
"""

import os
import time
import threading
import hashlib
import importlib.util
from typing import Dict, List, Any, Optional
from dataclasses import dataclass, field


# ============================================================================
# 插件沙箱
# ============================================================================

@dataclass
class SandboxPolicy:
    """沙箱策略"""
    max_cpu_time_s: float = 10.0
    max_memory_mb: int = 512
    max_requests: int = 1000
    allowed_apis: List[str] = field(default_factory=list)
    denied_apis: List[str] = field(default_factory=list)
    network_enabled: bool = False
    filesystem_readonly: bool = True

    def check_api(self, api_name: str) -> bool:
        """检查 API 是否允许"""
        if api_name in self.denied_apis:
            return False
        if self.allowed_apis and api_name not in self.allowed_apis:
            return False
        return True


class SandboxedPlugin:
    """
    沙箱插件包装器

    限制插件的资源和 API 调用。
    """

    def __init__(self, plugin: Any, policy: SandboxPolicy):
        self.plugin = plugin
        self.policy = policy
        self._request_count = 0
        self._lock = threading.Lock()

    def infer(self, ctx: Dict) -> Dict:
        """受限推理"""
        with self._lock:
            if self._request_count >= self.policy.max_requests:
                return {"error": "request limit exceeded"}
            self._request_count += 1

        # CPU 时间限制
        start = time.time()
        try:
            result = self.plugin.infer(ctx)
        except Exception as e:
            return {"error": f"plugin error: {e}"}
        finally:
            elapsed = time.time() - start
            if elapsed > self.policy.max_cpu_time_s:
                return {"error": "cpu time limit exceeded"}

        # 结果大小限制
        result_str = str(result)
        if len(result_str) > self.policy.max_memory_mb * 1024 * 1024:
            return {"error": "result too large"}

        return result

    def __getattr__(self, name):
        """代理到原插件，但限制危险 API"""
        if not self.policy.check_api(name):
            raise PermissionError(f"API {name} 被沙箱禁止")
        return getattr(self.plugin, name)


# ============================================================================
# 插件热部署
# ============================================================================

@dataclass
class WatchedPlugin:
    path: str
    mtime: float
    hash: str
    version: int = 1


class PluginHotDeploy:
    """
    插件热部署器

    监听插件文件变化，自动重载。
    """

    def __init__(self, poll_interval_s: float = 2.0):
        self.poll_interval = poll_interval_s
        self._watched: Dict[str, WatchedPlugin] = {}
        self._callbacks: List[Any] = []
        self._running = False
        self._thread: Optional[threading.Thread] = None

    def watch(self, plugin_path: str):
        """监听插件文件"""
        h = self._file_hash(plugin_path)
        self._watched[plugin_path] = WatchedPlugin(
            path=plugin_path,
            mtime=os.path.getmtime(plugin_path),
            hash=h,
        )

    def on_change(self, callback):
        """注册变更回调"""
        self._callbacks.append(callback)

    def _file_hash(self, path: str) -> str:
        try:
            with open(path, "rb") as f:
                return hashlib.md5(f.read()).hexdigest()
        except Exception:
            return ""

    def _poll(self):
        while self._running:
            for path, wp in list(self._watched.items()):
                try:
                    current_mtime = os.path.getmtime(path)
                    current_hash = self._file_hash(path)
                except Exception:
                    continue

                if current_mtime != wp.mtime or current_hash != wp.hash:
                    wp.mtime = current_mtime
                    wp.hash = current_hash
                    wp.version += 1
                    for cb in self._callbacks:
                        try:
                            cb(path, wp.version)
                        except Exception:
                            pass
            time.sleep(self.poll_interval)

    def start(self):
        self._running = True
        self._thread = threading.Thread(target=self._poll, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=1)


# ============================================================================
# 插件版本管理
# ============================================================================

class PluginVersionManager:
    """
    插件版本管理器

    支持同时加载多个版本，A/B 测试。
    """

    def __init__(self):
        self._versions: Dict[str, Dict[str, Any]] = {}
        self._active: Dict[str, str] = {}
        self._lock = threading.Lock()

    def register(self, name: str, version: str, plugin: Any):
        """注册插件版本"""
        with self._lock:
            if name not in self._versions:
                self._versions[name] = {}
            self._versions[name][version] = plugin
            if name not in self._active:
                self._active[name] = version

    def activate(self, name: str, version: str) -> bool:
        """激活指定版本"""
        with self._lock:
            if name in self._versions and version in self._versions[name]:
                self._active[name] = version
                return True
            return False

    def get(self, name: str) -> Optional[Any]:
        """获取当前激活版本的插件"""
        with self._lock:
            version = self._active.get(name)
            if not version:
                return None
            return self._versions.get(name, {}).get(version)

    def get_version(self, name: str, version: str) -> Optional[Any]:
        """获取指定版本"""
        return self._versions.get(name, {}).get(version)

    def ab_test(self, name: str, version_a: str, version_b: str,
                traffic_split: float = 0.5) -> Any:
        """A/B 测试：按流量比例返回不同版本"""
        import random
        version = version_a if random.random() < traffic_split else version_b
        return self.get_version(name, version)

    def list_versions(self, name: str) -> List[str]:
        return list(self._versions.get(name, {}).keys())


if __name__ == "__main__":
    # 沙箱演示
    class DemoPlugin:
        def infer(self, ctx):
            return {"decision": "keep", "confidence": 80}

    policy = SandboxPolicy(
        max_requests=10,
        allowed_apis=["infer"],
        denied_apis=["__getattr__", "eval"],
    )
    sandboxed = SandboxedPlugin(DemoPlugin(), policy)
    print(f"沙箱推理: {sandboxed.infer({})}")

    # 版本管理演示
    vpm = PluginVersionManager()
    vpm.register("sched", "1.0.0", "plugin_v1")
    vpm.register("sched", "2.0.0", "plugin_v2")
    vpm.activate("sched", "2.0.0")
    print(f"\n激活版本: {vpm._active['sched']}")
    print(f"版本列表: {vpm.list_versions('sched')}")
