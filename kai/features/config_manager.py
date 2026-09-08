# SPDX-License-Identifier: MIT
"""
config_manager.py — 配置验证 + 热更新 + 备份恢复（ROADMAP 5.2）

支持：
  - 配置验证（schema 校验）
  - 配置热更新（变更监听）
  - 配置备份 / 恢复
  - 配置版本历史

用法：
    from config_manager import ConfigManager
    cm = ConfigManager("/etc/kai-linux/config.yaml")
    cm.validate(config_dict)
    cm.backup()
    cm.restore(backup_path)
"""

import os
import time
import json
import yaml
import shutil
import threading
from typing import Dict, Any, Optional, List
from dataclasses import dataclass


# ============================================================================
# Schema 定义
# ============================================================================

CONFIG_SCHEMA = {
    "backend": {"type": str, "allowed": ["deepseek", "kimi"], "default": "deepseek"},
    "model": {"type": str, "default": "deepseek-chat"},
    "mode": {"type": str, "allowed": ["async", "sync", "override"], "default": "async"},
    "sched_threshold": {"type": int, "min": 0, "max": 10000, "default": 7000},
    "io_threshold": {"type": int, "min": 0, "max": 10000, "default": 7000},
    "sec_threshold": {"type": int, "min": 0, "max": 10000, "default": 8000},
    "api_timeout_ms": {"type": int, "min": 100, "max": 60000, "default": 5000},
    "cache_enabled": {"type": bool, "default": True},
    "cache_ttl_ms": {"type": int, "min": 0, "max": 3600000, "default": 5000},
    "log_level": {"type": str, "allowed": ["debug", "info", "warn", "error"], "default": "info"},
}


class ConfigValidationError(Exception):
    """配置验证错误"""
    pass


class ConfigManager:
    """
    配置管理器
    """

    def __init__(self, config_path: str,
                 backup_dir: Optional[str] = None):
        self.config_path = config_path
        self.backup_dir = backup_dir or os.path.join(
            os.path.dirname(config_path), "backups")
        self._config: Dict = {}
        self._lock = threading.Lock()
        self._watchers: List[Any] = []
        os.makedirs(self.backup_dir, exist_ok=True)
        self._load()

    def _load(self):
        """加载配置"""
        try:
            with open(self.config_path) as f:
                if self.config_path.endswith((".yaml", ".yml")):
                    self._config = yaml.safe_load(f) or {}
                else:
                    self._config = json.load(f)
        except FileNotFoundError:
            self._config = {}
        # 应用默认值
        self._apply_defaults()

    def _apply_defaults(self):
        """应用默认值"""
        for key, rule in CONFIG_SCHEMA.items():
            if key not in self._config:
                self._config[key] = rule.get("default")

    def validate(self, config: Optional[Dict] = None) -> List[str]:
        """
        验证配置，返回错误列表（空列表表示通过）
        """
        cfg = config or self._config
        errors = []

        for key, rule in CONFIG_SCHEMA.items():
            if key not in cfg:
                continue
            value = cfg[key]

            # 类型检查
            expected_type = rule["type"]
            if not isinstance(value, expected_type):
                # bool 是 int 子类，特殊处理
                if expected_type is bool and isinstance(value, bool):
                    pass
                elif expected_type is int and isinstance(value, bool):
                    errors.append(f"{key}: 期望 int，得到 bool")
                else:
                    errors.append(f"{key}: 期望 {expected_type.__name__}，"
                                  f"得到 {type(value).__name__}")
                continue

            # 枚举检查
            if "allowed" in rule and value not in rule["allowed"]:
                errors.append(f"{key}: {value} 不在 {rule['allowed']} 中")

            # 范围检查
            if isinstance(value, (int, float)):
                if "min" in rule and value < rule["min"]:
                    errors.append(f"{key}: {value} < 最小值 {rule['min']}")
                if "max" in rule and value > rule["max"]:
                    errors.append(f"{key}: {value} > 最大值 {rule['max']}")

        return errors

    def validate_or_raise(self, config: Optional[Dict] = None):
        errors = self.validate(config)
        if errors:
            raise ConfigValidationError("; ".join(errors))

    def get(self, key: str, default: Any = None) -> Any:
        return self._config.get(key, default)

    def set(self, key: str, value: Any, persist: bool = True):
        """设置配置（含热更新通知）"""
        self._config[key] = value
        if persist:
            self.save()
        self._notify(key, value)

    def update(self, updates: Dict, persist: bool = True):
        """批量更新"""
        for key, value in updates.items():
            self._config[key] = value
        if persist:
            self.save()
        for key, value in updates.items():
            self._notify(key, value)

    def save(self):
        """保存配置"""
        with self._lock:
            with open(self.config_path, "w") as f:
                if self.config_path.endswith((".yaml", ".yml")):
                    yaml.dump(self._config, f, allow_unicode=True,
                             sort_keys=False, default_flow_style=False)
                else:
                    json.dump(self._config, f, indent=2, ensure_ascii=False)

    def backup(self) -> str:
        """备份配置"""
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        backup_path = os.path.join(self.backup_dir, f"config_{timestamp}.yaml")
        shutil.copy2(self.config_path, backup_path)
        return backup_path

    def restore(self, backup_path: str) -> bool:
        """从备份恢复"""
        if not os.path.exists(backup_path):
            return False
        shutil.copy2(backup_path, self.config_path)
        self._load()
        return True

    def list_backups(self) -> List[Dict]:
        """列出所有备份"""
        backups = []
        if os.path.isdir(self.backup_dir):
            for f in sorted(os.listdir(self.backup_dir)):
                path = os.path.join(self.backup_dir, f)
                backups.append({
                    "file": f,
                    "size": os.path.getsize(path),
                    "mtime": os.path.getmtime(path),
                })
        return backups

    def watch(self, callback):
        """注册配置变更回调（热更新）"""
        self._watchers.append(callback)

    def _notify(self, key: str, value: Any):
        for cb in self._watchers:
            try:
                cb(key, value)
            except Exception:
                pass


if __name__ == "__main__":
    cm = ConfigManager("/tmp/kai_config.yaml")

    # 验证
    errors = cm.validate({
        "backend": "deepseek",
        "sched_threshold": 7000,
    })
    print(f"验证结果: {errors or '通过'}")

    # 无效配置
    errors = cm.validate({
        "backend": "invalid",
        "sched_threshold": 999999,
    })
    print(f"无效配置: {errors}")

    # 备份
    cm.save()
    backup = cm.backup()
    print(f"备份: {backup}")
