# SPDX-License-Identifier: MIT
"""
security.py — RBAC 访问控制 + API 密钥轮换（ROADMAP 4.2/4.3）

支持：
  - 基于角色的权限控制（RBAC）
  - API 密钥轮换
  - 权限校验
  - 审计

用法：
    from security import RBAC, KeyRotation
    rbac = RBAC()
    rbac.add_role("admin", ["infer", "model_load", "config"])
    rbac.add_user("alice", "admin")
    rbac.check("alice", "infer")
"""

import os
import time
import secrets
import hashlib
import threading
from typing import Dict, List, Set, Optional
from dataclasses import dataclass, field


# ============================================================================
# RBAC
# ============================================================================

@dataclass
class Role:
    name: str
    permissions: Set[str] = field(default_factory=set)
    inherits: List[str] = field(default_factory=list)


@dataclass
class User:
    name: str
    roles: List[str] = field(default_factory=list)


class RBAC:
    """
    基于角色的访问控制
    """

    # 权限定义
    PERMISSIONS = [
        "infer",          # 推理
        "model_load",     # 模型加载
        "model_unload",   # 模型卸载
        "config_read",    # 读配置
        "config_write",   # 写配置
        "plugin_manage",  # 插件管理
        "audit_read",     # 读审计
        "admin",          # 管理员
    ]

    def __init__(self):
        self._roles: Dict[str, Role] = {}
        self._users: Dict[str, User] = {}
        self._lock = threading.Lock()
        self._init_default_roles()

    def _init_default_roles(self):
        """初始化默认角色"""
        self.add_role("admin", all(self.PERMISSIONS))
        self.add_role("operator", ["infer", "model_load", "config_read",
                                   "audit_read"])
        self.add_role("viewer", ["infer", "config_read"])

    def add_role(self, name: str, permissions: List[str],
                 inherits: Optional[List[str]] = None):
        with self._lock:
            self._roles[name] = Role(
                name=name,
                permissions=set(permissions),
                inherits=inherits or [],
            )

    def remove_role(self, name: str):
        with self._lock:
            self._roles.pop(name, None)

    def add_user(self, name: str, roles: List[str]):
        with self._lock:
            self._users[name] = User(name=name, roles=roles)

    def remove_user(self, name: str):
        with self._lock:
            self._users.pop(name, None)

    def assign_role(self, username: str, role: str):
        with self._lock:
            user = self._users.get(username)
            if user and role not in user.roles:
                user.roles.append(role)

    def revoke_role(self, username: str, role: str):
        with self._lock:
            user = self._users.get(username)
            if user and role in user.roles:
                user.roles.remove(role)

    def _get_effective_permissions(self, role_name: str) -> Set[str]:
        """获取角色的有效权限（含继承）"""
        perms = set()
        role = self._roles.get(role_name)
        if not role:
            return perms

        # 递归处理继承
        for parent in role.inherits:
            perms.update(self._get_effective_permissions(parent))

        perms.update(role.permissions)
        return perms

    def get_user_permissions(self, username: str) -> Set[str]:
        """获取用户所有权限"""
        user = self._users.get(username)
        if not user:
            return set()
        perms = set()
        for role in user.roles:
            perms.update(self._get_effective_permissions(role))
        return perms

    def check(self, username: str, permission: str) -> bool:
        """检查用户是否有权限"""
        perms = self.get_user_permissions(username)
        return "admin" in perms or permission in perms

    def check_or_raise(self, username: str, permission: str):
        """检查权限，无权限则抛出异常"""
        if not self.check(username, permission):
            raise PermissionError(
                f"用户 {username} 没有权限 {permission}")

    def list_roles(self) -> Dict:
        with self._lock:
            return {
                name: {
                    "permissions": list(r.permissions),
                    "inherits": r.inherits,
                }
                for name, r in self._roles.items()
            }

    def list_users(self) -> Dict:
        with self._lock:
            return {
                name: list(u.roles)
                for name, u in self._users.items()
            }


# ============================================================================
# API 密钥轮换
# ============================================================================

class KeyRotation:
    """
    API 密钥轮换管理器

    支持：
      - 密钥生成
      - 定期轮换
      - 密钥过期
      - 旧密钥过渡期
    """

    def __init__(self, rotation_interval_s: int = 86400 * 30):
        self._keys: Dict[str, Dict] = {}
        self._rotation_interval = rotation_interval_s
        self._lock = threading.Lock()

    def generate_key(self, name: str) -> str:
        """生成新密钥"""
        key = "sk-" + secrets.token_urlsafe(32)
        with self._lock:
            self._keys[name] = {
                "key": key,
                "created_at": time.time(),
                "expires_at": time.time() + self._rotation_interval,
                "active": True,
            }
        return key

    def rotate(self, name: str) -> str:
        """轮换密钥（生成新密钥，旧密钥进入过渡期）"""
        new_key = self.generate_key(name)
        with self._lock:
            old = self._keys.get(name)
            if old:
                old["active"] = False
                old["retired_at"] = time.time()
        return new_key

    def get_active_key(self, name: str) -> Optional[str]:
        """获取当前有效密钥"""
        with self._lock:
            entry = self._keys.get(name)
            if entry and entry["active"] and time.time() < entry["expires_at"]:
                return entry["key"]
            return None

    def is_expired(self, name: str) -> bool:
        with self._lock:
            entry = self._keys.get(name)
            if not entry:
                return True
            return time.time() > entry["expires_at"]

    def validate(self, name: str, key: str) -> bool:
        """验证密钥（含过渡期旧密钥）"""
        with self._lock:
            for entry in self._keys.values():
                if entry["key"] == key:
                    return time.time() < entry["expires_at"]
            return False

    def status(self) -> Dict:
        with self._lock:
            return {
                name: {
                    "active": e["active"],
                    "expires_in_s": int(e["expires_at"] - time.time()),
                }
                for name, e in self._keys.items()
            }


if __name__ == "__main__":
    # RBAC 演示
    rbac = RBAC()
    rbac.add_user("alice", ["admin"])
    rbac.add_user("bob", ["viewer"])
    print(f"alice 可推理: {rbac.check('alice', 'infer')}")
    print(f"bob 可加载模型: {rbac.check('bob', 'model_load')}")

    # 密钥轮换演示
    kr = KeyRotation(rotation_interval_s=60)
    key = kr.generate_key("deepseek")
    print(f"\n生成的密钥: {key[:10]}...")
    new_key = kr.rotate("deepseek")
    print(f"轮换后密钥: {new_key[:10]}...")
