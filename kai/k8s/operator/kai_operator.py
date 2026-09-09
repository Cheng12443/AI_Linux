# SPDX-License-Identifier: MIT
"""
operator.py — KAI Linux Kubernetes Operator（ROADMAP 7.3）

职责：
  - 监控 KAI 节点状态
  - 自动故障转移
  - 滚动更新
  - 告警联动

完整实现需 kopf/fabric8 等框架，这里提供核心逻辑框架。
"""

import time
import json
import threading
from typing import Dict, List, Optional, Any


class KAIOperator:
    """
    KAI Linux Operator 核心逻辑
    """

    def __init__(self, namespace: str = "kai-system"):
        self.namespace = namespace
        self._nodes: Dict[str, Dict] = {}
        self._running = False
        self._thread = None

    # ------------------------------------------------------------------
    # 节点管理
    # ------------------------------------------------------------------

    def register_node(self, name: str, backend: str = "deepseek",
                      api_key_env: str = "") -> bool:
        """注册节点"""
        self._nodes[name] = {
            "name": name,
            "backend": backend,
            "api_key_env": api_key_env,
            "status": "pending",
            "health": 0,
            "last_seen": time.time(),
            "inferences": 0,
            "errors": 0,
        }
        return True

    def deregister_node(self, name: str) -> bool:
        """注销节点"""
        return bool(self._nodes.pop(name, None))

    def heartbeat(self, name: str, health: int = 100,
                  inferences: int = 0, errors: int = 0) -> bool:
        """节点心跳"""
        node = self._nodes.get(name)
        if not node:
            return False
        node["status"] = "healthy" if health >= 60 else "degraded"
        node["health"] = health
        node["last_seen"] = time.time()
        node["inferences"] += inferences
        node["errors"] += errors
        return True

    def _check_stale(self, timeout_s: float = 60.0):
        """标记失联节点"""
        now = time.time()
        for name, node in self._nodes.items():
            if now - node["last_seen"] > timeout_s:
                if node["status"] != "offline":
                    print(f"[operator] 节点 {name} 失联 → offline")
                node["status"] = "offline"

    # ------------------------------------------------------------------
    # 故障转移
    # ------------------------------------------------------------------

    def failover(self, failed_node: str) -> Optional[str]:
        """故障转移：找一个健康节点接管"""
        for name, node in sorted(
                self._nodes.items(),
                key=lambda x: x[1]["health"], reverse=True):
            if name != failed_node and node["status"] in ("healthy", "degraded"):
                print(f"[operator] {failed_node} → {name} 故障转移")
                return name
        print(f"[operator] 无可用节点接管 {failed_node}")
        return None

    def rolling_update(self, new_config: Dict,
                       batch_size: int = 1) -> Dict[str, str]:
        """滚动更新"""
        results = {}
        for i, name in enumerate(self._nodes):
            # 逐个节点更新
            node = self._nodes[name]
            node["config"] = new_config
            results[name] = "updated"
        return results

    # ------------------------------------------------------------------
    # 监控循环
    # ------------------------------------------------------------------

    def _monitor_loop(self):
        """后台监控"""
        while self._running:
            self._check_stale()
            time.sleep(10)

    def start_monitor(self):
        """启动监控"""
        self._running = True
        self._thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self._thread.start()

    def stop_monitor(self):
        """停止监控"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=1)

    # ------------------------------------------------------------------
    # 状态
    # ------------------------------------------------------------------

    def status(self) -> Dict:
        return {
            "namespace": self.namespace,
            "nodes": {
                name: {k: v for k, v in node.items()
                       if k not in ("api_key_env",)}
                for name, node in self._nodes.items()
            },
            "healthy_nodes": sum(1 for n in self._nodes.values()
                                if n["status"] == "healthy"),
            "total_nodes": len(self._nodes),
        }


if __name__ == "__main__":
    op = KAIOperator()

    # 模拟节点
    op.register_node("kai-node-1", backend="deepseek")
    op.register_node("kai-node-2", backend="kimi")
    op.register_node("kai-node-3", backend="deepseek")

    op.start_monitor()
    op.heartbeat("kai-node-1", health=95)
    op.heartbeat("kai-node-2", health=88)
    op.heartbeat("kai-node-3", health=20)

    # 节点 3 故障 → 转移
    op.failover("kai-node-3")
    op.stop_monitor()

    print(json.dumps(op.status(), indent=2, ensure_ascii=False))
