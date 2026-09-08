# SPDX-License-Identifier: MIT
"""
AI Linux Python SDK

提供对 AI Linux 系统的 Pythonic 编程接口。

安装：
    pip install ai-linux-sdk

    或直接复制 ai_linux/ 目录到项目

基本用法：
    from ai_linux import AILinux

    ai = AILinux()
    response = ai.ask("分析系统负载")
    print(response.text)
"""

from .client import AILinux, AIClient
from .config import AIConfig
from .models import AIResponse, ChatMessage, SystemStatus
from .exceptions import AIError, ConfigError, TimeoutError

__version__ = "1.0.0"
__all__ = [
    "AILinux",
    "AIClient",
    "AIConfig",
    "AIResponse",
    "ChatMessage",
    "SystemStatus",
    "AIError",
    "ConfigError",
    "TimeoutError",
]
