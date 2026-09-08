# SPDX-License-Identifier: MIT
"""
ai_linux/client.py — Python SDK 核心客户端
"""

import os
import json
import time
import threading
import queue
import socket
import struct
import hashlib
import logging
from typing import Optional, List, Dict, Any, Callable, Union
from dataclasses import dataclass, field, asdict
from enum import Enum
from urllib.parse import urlparse

try:
    import websocket
    HAS_WEBSOCKET = True
except ImportError:
    HAS_WEBSOCKET = False

logger = logging.getLogger(__name__)

def _mask_key(key: str) -> str:
    """脱敏 API 密钥"""
    if not key:
        return "***"
    if len(key) <= 8:
        return "***"
    return key[:4] + "***" + key[-4:]




# ============================================================================
# 数据模型
# ============================================================================

class MessageRole(str, Enum):
    USER = "user"
    AI = "ai"
    SYSTEM = "system"
    TOOL = "tool"


@dataclass
class ChatMessage:
    """对话消息"""
    role: str
    content: str
    timestamp: float = field(default_factory=time.time)
    backend: Optional[str] = None
    confidence: Optional[int] = None
    metadata: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict:
        return asdict(self)


@dataclass
class AIResponse:
    """AI 响应"""
    text: str
    backend: str
    confidence: int
    latency_ms: float
    model: str
    request_id: str = ""
    metadata: Dict[str, Any] = field(default_factory=dict)

    def __str__(self) -> str:
        return self.text

    def to_dict(self) -> Dict:
        return asdict(self)


@dataclass
class SystemStatus:
    """系统状态"""
    cpu_usage: int
    mem_usage: int
    loadavg: str
    uptime: str
    net_rx: str
    net_tx: str
    timestamp: float = field(default_factory=time.time)
    ai_inferences: int = 0
    ai_errors: int = 0
    layer2_connected: bool = False
    backend: str = ""

    def to_dict(self) -> Dict:
        return asdict(self)


class DecisionDomain(str, Enum):
    """决策域"""
    SCHEDULING = "scheduling"
    IO_NETWORK = "io_network"
    SECURITY = "security"
    MEMORY = "memory"
    SYSTEM = "system"


@dataclass
class SchedDecision:
    """调度决策"""
    action: str  # promote / demote / migrate / batch / keep
    confidence: int  # 0-100
    target_cpu: Optional[int] = None
    reason: str = ""
    override: bool = False

    def to_dict(self) -> Dict:
        return asdict(self)


# ============================================================================
# HTTP 客户端（纯 Python，无依赖）
# ============================================================================

class HttpClient:
    """纯 Python HTTP 客户端，不依赖 requests"""

    def __init__(self, timeout: int = 30):
        self.timeout = timeout

    def post_json(self, url: str, headers: Dict, body: Dict) -> Dict:
        parsed = urlparse(url)
        host = parsed.netloc
        path = parsed.path
        if parsed.query:
            path += "?" + parsed.query

        # 确定端口
        if ":" in host:
            h, port_str = host.rsplit(":", 1)
            host = h
            port = int(port_str)
        else:
            port = 443 if parsed.scheme == "https" else 80

        # 建立连接
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self.timeout)

        if parsed.scheme == "https":
            import ssl
            ctx = ssl.create_default_context()
            sock = ctx.wrap_socket(sock, server_hostname=host)

        sock.connect((host, port))

        # 发送请求
        body_str = json.dumps(body)
        request = (
            f"POST {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            f"Authorization: Bearer {headers.get('Authorization', '').split(' ', 1)[1]}\r\n"
            f"Content-Type: application/json\r\n"
            f"Content-Length: {len(body_str)}\r\n"
            f"User-Agent: AI-Linux-SDK/1.0\r\n"
            f"Connection: close\r\n"
            f"\r\n"
            f"{body_str}"
        )

        sock.sendall(request.encode())

        # 接收响应
        chunks = []
        while True:
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
            except socket.timeout:
                break
        sock.close()

        raw = b"".join(chunks).decode("utf-8", errors="replace")

        # 解析 HTTP 响应
        header_end = raw.find("\r\n\r\n")
        if header_end == -1:
            raise AIError("无效的 HTTP 响应")

        body = raw[header_end + 4:]

        # 简单解析 JSON
        try:
            return json.loads(body)
        except json.JSONDecodeError:
            # 尝试提取 error
            raise AIError(f"API 返回格式错误: {body[:200]}")


# ============================================================================
# WebSocket 实时推送
# ============================================================================

class WebSocketClient:
    """
    WebSocket 客户端，支持实时 AI 推理推送

    用法：
        ws = WebSocketClient("ws://localhost:9999/ws")
        ws.connect()
        ws.subscribe("sched_decision", callback)
        ws.send_inference_request(...)
    """

    def __init__(self, url: str,
                 api_key: Optional[str] = None,
                 on_message: Optional[Callable[[Dict], None]] = None,
                 on_connect: Optional[Callable[[], None]] = None,
                 on_disconnect: Optional[Callable[[], None]] = None):
        self.url = url
        self.api_key = api_key or os.getenv("DEEPSEEK_API_KEY") or ""
        self.on_message = on_message
        self.on_connect = on_connect
        self.on_disconnect = on_disconnect

        self.ws = None
        self.running = False
        self.thread = None
        self.subscriptions: Dict[str, Callable] = {}
        self._lock = threading.Lock()

    def _start_ws_thread(self):
        if not HAS_WEBSOCKET:
            logger.warning("websocket-client 未安装，WebSocket 功能不可用")
            return

        import websocket

        headers = []
        if self.api_key:
            headers.append(f"Authorization: Bearer {self.api_key}")

        self.ws = websocket.WebSocketApp(
            self.url,
            header=headers,
            on_message=self._on_ws_message,
            on_open=self._on_ws_open,
            on_error=self._on_ws_error,
            on_close=self._on_ws_close,
        )

        self.running = True
        self.ws.run_forever(ping_interval=30, ping_timeout=10)

    def _on_ws_open(self, ws):
        logger.info("WebSocket 已连接")
        if self.on_connect:
            self.on_connect()

    def _on_ws_message(self, ws, message):
        try:
            data = json.loads(message)
            msg_type = data.get("type", "")

            # 检查订阅
            with self._lock:
                callback = self.subscriptions.get(msg_type)

            if callback:
                callback(data)
            elif self.on_message:
                self.on_message(data)

        except json.JSONDecodeError:
            logger.warning(f"WebSocket 收到无效 JSON: {message[:100]}")

    def _on_ws_error(self, ws, error):
        logger.error(f"WebSocket 错误: {error}")

    def _on_ws_close(self, ws, code, reason):
        logger.info(f"WebSocket 断开: {code} {reason}")
        if self.on_disconnect:
            self.on_disconnect()

    def connect(self):
        """启动 WebSocket 连接"""
        self.thread = threading.Thread(target=self._start_ws_thread, daemon=True)
        self.thread.start()

    def disconnect(self):
        """断开连接"""
        self.running = False
        if self.ws:
            self.ws.close()

    def subscribe(self, event_type: str, callback: Callable[[Dict], None]):
        """
        订阅事件

        事件类型：
            - sched_decision: 调度决策
            - io_decision: IO 决策
            - security_decision: 安全决策
            - telemetry: 系统遥测
            - layer2_response: Layer2 推理响应
        """
        with self._lock:
            self.subscriptions[event_type] = callback

    def send_inference_request(self,
                               prompt: str,
                               domain: str = "system",
                               request_id: Optional[str] = None) -> str:
        """
        发送推理请求（通过 WebSocket）

        返回请求 ID
        """
        if not self.ws or not self.running:
            raise AIError("WebSocket 未连接")

        if not request_id:
            request_id = hashlib.md5(f"{prompt}{time.time()}".encode()).hexdigest()[:16]

        msg = {
            "type": "inference_request",
            "id": request_id,
            "prompt": prompt,
            "domain": domain,
            "timestamp": time.time(),
        }

        self.ws.send(json.dumps(msg))
        return request_id


# ============================================================================
# 主客户端
# ============================================================================

class AIClient:
    """
    AI Linux Python SDK 主客户端

    支持三种后端：
        - DeepSeek (默认)
        - Kimi K3 (Moonshot AI)

    支持三种连接方式：
        - 直接 HTTP API 调用
        - Layer2 网关（通过 socket/netlink）
        - WebSocket 实时推送

    用法：
        ai = AIClient()
        response = ai.ask("分析系统负载")

        ai = AIClient(backend="kimi")
        response = ai.ask("安全检测")
    """

    def __init__(self,
                 api_key: Optional[str] = None,
                 backend: str = "deepseek",
                 model: Optional[str] = None,
                 timeout: int = 30,
                 temperature: float = 0.3,
                 max_tokens: int = 512,
                 cache_enabled: bool = True,
                 layer2_url: Optional[str] = None,
                 ws_url: Optional[str] = None):
        self.api_key = api_key or os.getenv("DEEPSEEK_API_KEY") or ""
        self.backend = backend.lower()
        self.timeout = timeout
        self.temperature = temperature
        self.max_tokens = max_tokens
        self.cache_enabled = cache_enabled
        self.layer2_url = layer2_url
        self.ws_url = ws_url

        # 选择 API 端点
        if self.backend == "kimi":
            self.api_url = "https://api.moonshot.cn/v1/chat/completions"
            self.model = model or "moonshot-v1-8k"
            self.kimi_api_key = self.api_key or os.getenv("KIMI_API_KEY") or ""
            self.active_key = self.kimi_api_key
        else:
            self.api_url = "https://api.deepseek.com/v1/chat/completions"
            self.model = model or "deepseek-chat"
            self.active_key = self.api_key

        if not self.active_key:
            raise AIError(
                f"未设置 API 密钥。"
                f"请设置 {'KIMI_API_KEY' if self.backend == 'kimi' else 'DEEPSEEK_API_KEY'} "
                f"环境变量，或在构造函数中传入 api_key 参数"
            )

        self.http = HttpClient(timeout=timeout)

        # 请求缓存
        self._cache: Dict[str, AIResponse] = {}
        self._cache_lock = threading.Lock()

        # WebSocket
        self.ws: Optional[WebSocketClient] = None

        # 对话历史
        self._history: List[ChatMessage] = []

    # ------------------------------------------------------------------------
    # 核心 API
    # ------------------------------------------------------------------------

    def ask(self,
            prompt: str,
            system: Optional[str] = None,
            use_cache: bool = True,
            max_retries: int = 3) -> AIResponse:
        """
        发送问题，获取 AI 响应

        参数：
            prompt: 你的问题
            system: 系统提示词（可选）
            use_cache: 是否使用缓存
            max_retries: 最大重试次数

        返回：
            AIResponse 对象

        用法：
            response = ai.ask("分析系统调度策略")
            print(response.text)
            print(f"后端: {response.backend}, 延迟: {response.latency_ms}ms")
        """
        # 缓存检查
        cache_key = hashlib.md5(prompt.encode()).hexdigest()
        if use_cache and self.cache_enabled:
            with self._cache_lock:
                if cache_key in self._cache:
                    return self._cache[cache_key]

        # 准备消息
        messages = []
        if system:
            messages.append({"role": "system", "content": system})
        if self._history:
            for m in self._history[-10:]:
                messages.append({"role": m.role, "content": m.content})
        messages.append({"role": "user", "content": prompt})

        # 构建请求体
        body = {
            "model": self.model,
            "messages": messages,
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
        }

        headers = {
            "Authorization": f"Bearer {self.active_key}",
        }

        # 重试循环
        last_error = None
        for attempt in range(max_retries):
            start = time.time()
            try:
                result = self.http.post_json(self.api_url, headers, body)
                latency_ms = (time.time() - start) * 1000

                # 提取响应
                choices = result.get("choices", [])
                if not choices:
                    raise AIError(f"API 返回为空: {result}")

                content = choices[0].get("message", {}).get("content", "")

                response = AIResponse(
                    text=content,
                    backend=self.backend,
                    confidence=80,
                    latency_ms=latency_ms,
                    model=self.model,
                    request_id=cache_key,
                )

                # 缓存
                if use_cache and self.cache_enabled:
                    with self._cache_lock:
                        self._cache[cache_key] = response

                # 记录历史
                self._history.append(ChatMessage(role="user", content=prompt))
                self._history.append(ChatMessage(
                    role="ai",
                    content=content,
                    backend=self.backend,
                    confidence=response.confidence,
                ))

                return response

            except (AIError, socket.error, ConnectionError) as e:
                last_error = e
                if attempt < max_retries - 1:
                    time.sleep(0.5 * (attempt + 1))

        raise AIError(f"请求失败（{max_retries} 次重试后）: {last_error}")

    def clear_history(self):
        """清空对话历史"""
        self._history.clear()

    # ------------------------------------------------------------------------
    # 便捷方法
    # ------------------------------------------------------------------------

    def sched_analyze(self,
                     pid: Optional[int] = None,
                     question: Optional[str] = None) -> AIResponse:
        """
        调度分析

        用法：
            response = ai.sched_analyze(pid=1234)
            response = ai.sched_analyze(question="CPU 使用率过高怎么办")
        """
        if pid:
            prompt = (
                f"分析 PID {pid} 的进程调度状态。"
                f"查看 /proc/{pid}/stat 和 /proc/{pid}/status，"
                f"给出 CPU 使用、上下文切换率、IO 等待等指标，"
                f"判断是否需要调整调度策略（升权/降权/迁移/批处理）。"
            )
        elif question:
            prompt = f"你是一个 Linux 内核调度专家。{question}"
        else:
            prompt = (
                "分析当前系统所有进程的调度状态。"
                "查看 /proc/stat 和 /proc/*/stat，"
                "找出 CPU 使用率最高的前 10 个进程，"
                "给出优化建议。"
            )

        return self.ask(prompt)

    def net_analyze(self,
                    connections: bool = False,
                    scan: bool = False,
                    question: Optional[str] = None) -> AIResponse:
        """
        网络分析

        用法：
            response = ai.net_analyze(connections=True)
            response = ai.net_analyze(scan=True)
        """
        if connections:
            prompt = (
                "分析当前网络连接状态。"
                "查看 /proc/net/tcp、/proc/net/udp、/proc/net/netstat，"
                "统计各状态的连接数（ESTABLISHED、TIME_WAIT、LISTEN 等），"
                "分析是否存在异常连接。"
            )
        elif scan:
            prompt = (
                "对系统网络进行安全扫描。"
                "分析 /proc/net/* 和 ss -s 输出，"
                "检查可疑的对外连接、非预期端口监听、异常流量模式。"
            )
        elif question:
            prompt = f"你是一个网络安全专家。{question}"
        else:
            prompt = "分析当前网络有什么异常？"

        return self.ask(prompt)

    def security_scan(self,
                     pid: Optional[int] = None,
                     full: bool = False,
                     question: Optional[str] = None) -> AIResponse:
        """
        安全扫描

        用法：
            response = ai.security_scan(pid=1234)
            response = ai.security_scan(full=True)
        """
        if pid:
            prompt = (
                f"对 PID {pid} 的进程进行安全检测。"
                f"分析 /proc/{pid}/cmdline、/proc/{pid}/maps、/proc/{pid}/fd，"
                f"检测：提权行为、敏感文件访问、LD_PRELOAD 注入、可疑映射、异常网络连接。"
                f"返回 JSON：{{\"threat_level\":\"low|medium|high|critical\","
                f"\"action\":\"allow|block|alert\"}}"
            )
        elif full:
            prompt = (
                "对整台服务器进行全面的安全检测：\n"
                "1. 检查异常进程（CPU 异常高、隐藏进程）\n"
                "2. 检查网络异常（可疑对外连接、端口扫描）\n"
                "3. 检查文件完整性（/etc/passwd、/bin/ls 等关键文件）\n"
                "4. 检查 crontab 和 systemd 服务\n"
                "5. 检查 .ssh/authorized_keys\n"
                "返回详细的安全报告，标注高危项。"
            )
        elif question:
            prompt = f"你是一个安全专家。{question}"
        else:
            prompt = "当前系统有什么安全风险？"

        return self.ask(prompt)

    def memory_analyze(self, full: bool = False) -> AIResponse:
        """
        内存分析
        """
        if full:
            prompt = (
                "分析系统内存使用情况。"
                "查看 /proc/meminfo、/proc/vmstat、/proc/buddyinfo，"
                "分析：内存使用率、Swap 使用率和换页率、内存碎片化程度、"
                "消耗内存最多的进程（排序）、页面换入/换出预测，"
                "给出优化建议。"
            )
        else:
            prompt = "当前内存使用情况如何？有哪些优化建议？"

        return self.ask(prompt)

    def code_generate(self, requirement: str, language: str = "shell") -> AIResponse:
        """
        代码生成

        用法：
            response = ai.code_generate(
                "监控 CPU/内存/磁盘，当超过阈值时发邮件",
                language="shell"
            )
            print(response.text)
        """
        prompt = (
            f"你是一个 Linux 系统程序员。"
            f"根据以下需求生成 {language} 代码：\n\n{requirement}\n\n"
            f"只返回代码，不要其他解释。代码要完整、可直接运行，"
            f"包含必要的错误处理。"
        )
        return self.ask(prompt, system="你是一个 Linux 系统程序员，生成高质量、可直接运行的脚本。")

    def orchestrate(self, requirement: str) -> AIResponse:
        """
        系统编排：多步骤综合任务

        用法：
            response = ai.orchestrate(
                "分析系统瓶颈，从调度、网络、内存三个维度给出优化方案"
            )
        """
        prompt = (
            f"你是一个 Linux 系统架构师。"
            f"请分析以下需求，制定综合优化方案：\n\n{requirement}\n\n"
            f"方案应包含：问题诊断、优化步骤、执行顺序、预期效果。"
        )
        return self.ask(prompt)

    # ------------------------------------------------------------------------
    # WebSocket
    # ------------------------------------------------------------------------

    def ws_connect(self):
        """建立 WebSocket 连接"""
        if not self.ws_url:
            self.ws_url = "ws://localhost:9999/ws"

        self.ws = WebSocketClient(
            url=self.ws_url,
            api_key=self.active_key,
        )
        self.ws.connect()

    def ws_subscribe(self, event: str, callback: Callable[[Dict], None]):
        """订阅 WebSocket 事件"""
        if self.ws:
            self.ws.subscribe(event, callback)

    def ws_disconnect(self):
        """断开 WebSocket"""
        if self.ws:
            self.ws.disconnect()

    # ------------------------------------------------------------------------
    # 上下文管理器
    # ------------------------------------------------------------------------

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.clear_history()
        self.ws_disconnect()


# ============================================================================
# 别名
# ============================================================================

AILinux = AIClient
