# SPDX-License-Identifier: MIT
"""
session.py — 会话上下文管理（ROADMAP 2.2 会话上下文）

支持：
  - 多轮对话记忆
  - 会话隔离
  - 上下文窗口（自动裁剪）
  - 会话持久化
  - Token 估算

用法：
    from session import SessionManager
    sm = SessionManager()
    sid = sm.create_session("系统诊断")
    sm.append(sid, "user", "分析负载")
    sm.append(sid, "ai", "负载正常")
    ctx = sm.get_context(sid)
"""

import time
import json
import hashlib
import threading
from typing import Dict, List, Any, Optional
from dataclasses import dataclass, field, asdict


@dataclass
class Message:
    role: str          # user / ai / system / tool
    content: str
    timestamp: float = field(default_factory=time.time)
    tokens: int = 0

    def to_dict(self) -> Dict:
        return asdict(self)


@dataclass
class Session:
    session_id: str
    title: str = ""
    messages: List[Message] = field(default_factory=list)
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)
    max_messages: int = 50
    max_tokens: int = 8000
    metadata: Dict = field(default_factory=dict)

    def to_dict(self) -> Dict:
        d = asdict(self)
        return d


def estimate_tokens(text: str) -> int:
    """粗略估算 token 数（中文约 1 字 = 1 token，英文约 4 字符 = 1 token）"""
    if not text:
        return 0
    # 简单启发式
    chinese = sum(1 for c in text if '\u4e00' <= c <= '\u9fff')
    other = len(text) - chinese
    return chinese + other // 4


class SessionManager:
    """
    会话管理器
    """

    def __init__(self, persist_path: Optional[str] = None):
        self._sessions: Dict[str, Session] = {}
        self._lock = threading.Lock()
        self._persist_path = persist_path
        if persist_path:
            self._load()

    def create_session(self, title: str = "",
                       max_messages: int = 50,
                       max_tokens: int = 8000) -> str:
        """创建会话，返回 session_id"""
        sid = hashlib.md5(f"{title}{time.time()}".encode()).hexdigest()[:16]
        with self._lock:
            self._sessions[sid] = Session(
                session_id=sid,
                title=title or f"session_{sid[:8]}",
                max_messages=max_messages,
                max_tokens=max_tokens,
            )
        return sid

    def append(self, session_id: str, role: str, content: str):
        """追加消息"""
        with self._lock:
            sess = self._sessions.get(session_id)
            if not sess:
                return
            msg = Message(role=role, content=content,
                          tokens=estimate_tokens(content))
            sess.messages.append(msg)
            sess.updated_at = time.time()
            self._trim(sess)

    def _trim(self, sess: Session):
        """裁剪超出限制的消息"""
        # 按条数裁剪
        while len(sess.messages) > sess.max_messages:
            sess.messages.pop(0)
        # 按 token 裁剪
        total = sum(m.tokens for m in sess.messages)
        while total > sess.max_tokens and len(sess.messages) > 2:
            removed = sess.messages.pop(0)
            total -= removed.tokens

    def get_context(self, session_id: str,
                    max_recent: int = 10) -> List[Dict]:
        """获取上下文（最近 N 条）"""
        with self._lock:
            sess = self._sessions.get(session_id)
            if not sess:
                return []
            recent = sess.messages[-max_recent:]
            return [m.to_dict() for m in recent]

    def get_messages(self, session_id: str) -> List[Message]:
        with self._lock:
            sess = self._sessions.get(session_id)
            return list(sess.messages) if sess else []

    def clear(self, session_id: str):
        with self._lock:
            sess = self._sessions.get(session_id)
            if sess:
                sess.messages.clear()

    def delete(self, session_id: str):
        with self._lock:
            self._sessions.pop(session_id, None)

    def list_sessions(self) -> List[Dict]:
        with self._lock:
            return [
                {
                    "session_id": s.session_id,
                    "title": s.title,
                    "messages": len(s.messages),
                    "updated_at": s.updated_at,
                }
                for s in self._sessions.values()
            ]

    def get_session(self, session_id: str) -> Optional[Session]:
        return self._sessions.get(session_id)

    def token_usage(self, session_id: str) -> int:
        sess = self._sessions.get(session_id)
        if not sess:
            return 0
        return sum(m.tokens for m in sess.messages)

    def _persist(self):
        if not self._persist_path:
            return
        with self._lock:
            data = {k: v.to_dict() for k, v in self._sessions.items()}
        try:
            with open(self._persist_path, "w") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def _load(self):
        try:
            with open(self._persist_path) as f:
                data = json.load(f)
            for sid, d in data.items():
                sess = Session(
                    session_id=d["session_id"],
                    title=d["title"],
                    created_at=d["created_at"],
                    updated_at=d["updated_at"],
                    max_messages=d.get("max_messages", 50),
                    max_tokens=d.get("max_tokens", 8000),
                    metadata=d.get("metadata", {}),
                )
                sess.messages = [Message(**m) for m in d.get("messages", [])]
                self._sessions[sid] = sess
        except Exception:
            pass

    def save(self):
        """持久化保存"""
        self._persist()


if __name__ == "__main__":
    sm = SessionManager()
    sid = sm.create_session("系统诊断")
    sm.append(sid, "system", "你是 Linux 诊断助手")
    sm.append(sid, "user", "分析 CPU 负载")
    sm.append(sid, "ai", "负载正常，平均 45%")

    print(f"会话: {sid}")
    print(f"Token 用量: {sm.token_usage(sid)}")
    print(f"上下文:")
    for m in sm.get_context(sid):
        print(f"  [{m['role']}] {m['content'][:30]}")
