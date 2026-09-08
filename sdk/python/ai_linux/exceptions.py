# SPDX-License-Identifier: MIT
"""
ai_linux/exceptions.py — 异常定义
"""


class AIError(Exception):
    """AI Linux SDK 基础异常"""
    pass


class ConfigError(AIError):
    """配置错误"""
    pass


class TimeoutError(AIError):
    """请求超时"""
    pass


class AuthenticationError(AIError):
    """认证失败（API 密钥无效）"""
    pass


class RateLimitError(AIError):
    """速率限制"""
    pass


class ValidationError(AIError):
    """参数校验失败"""
    pass


class NetworkError(AIError):
    """网络错误"""
    pass


class Layer2Error(AIError):
    """Layer2 网关错误"""
    pass


class WebSocketError(AIError):
    """WebSocket 错误"""
    pass
