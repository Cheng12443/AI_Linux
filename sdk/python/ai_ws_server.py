#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
ai_ws_server.py — AI Linux WebSocket 推送服务器
#
# 启动后提供 WebSocket 接口，支持：
#   - 实时 AI 推理推送
#   - 系统遥测推送
#   - 调度/IO/安全决策事件
#
# 编译（依赖 ws）：
#   pip install websockets
#
# 运行：
#   DEEPSEEK_API_KEY=xxx python3 ai_ws_server.py
#
# WebSocket 端点：
#   ws://localhost:9998/
#
# JavaScript 客户端示例：
#   const ws = new WebSocket("ws://localhost:9998/");
#   ws.onmessage = (e) => console.log(JSON.parse(e.data));
#   ws.send(JSON.stringify({
#     type: "subscribe",
#     events: ["sched_decision", "telemetry", "security_alert"]
#   }));
"""

import os
import sys
import json
import time
import asyncio
import logging
import hashlib
from pathlib import Path
from typing import Dict, List, Set, Optional, Any
from dataclasses import dataclass, asdict, field
from enum import Enum
import threading

try:
    import websockets
    HAS_WEBSOCKETS = True
except ImportError:
    HAS_WEBSOCKETS = False
    print("⚠️  websockets 未安装，运行: pip install websockets")

try:
    import yaml
except ImportError:
    yaml = None

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s: %(message)s"
)
logger = logging.getLogger("ai-ws")


# ============================================================================
# 配置
# ============================================================================

class Config:
    PORT = 9998
    HOST = "0.0.0.0"

    DEEPSEEK_KEY = os.getenv("DEEPSEEK_API_KEY", "")
    KIMI_KEY = os.getenv("KIMI_API_KEY", "")

    TELEMETRY_INTERVAL = 5.0    # 遥测推送间隔（秒）
    HEARTBEAT_INTERVAL = 30.0   # 心跳间隔（秒）

    # 默认后端
    DEFAULT_BACKEND = "deepseek"
    DEFAULT_MODEL = "deepseek-chat"

    @classmethod
    def load(cls, path: str):
        """从 YAML 加载配置"""
        if not yaml or not Path(path).exists():
            return
        with open(path) as f:
            d = yaml.safe_load(f)
        if not d:
            return
        cls.PORT = d.get("port", cls.PORT)
        cls.HOST = d.get("host", cls.HOST)


# ============================================================================
# 数据模型
# ============================================================================

@dataclass
class ChatMessage:
    role: str
    content: str
    timestamp: float = field(default_factory=time.time)
    backend: str = ""


@dataclass
class SystemTelemetry:
    """系统遥测"""
    cpu_usage: int = 0
    mem_usage: int = 0
    load1: float = 0.0
    load5: float = 0.0
    uptime_s: float = 0.0
    net_rx: int = 0
    net_tx: int = 0
    ai_inferences: int = 0
    layer2_connected: int = 0
    timestamp: float = field(default_factory=time.time)


@dataclass
class SchedDecisionEvent:
    """调度决策事件"""
    decision: str = "keep"
    confidence: int = 0
    pid: int = 0
    reason: str = ""
    backend: str = ""
    latency_ms: float = 0.0
    timestamp: float = field(default_factory=time.time)


@dataclass
class SecurityAlert:
    """安全告警"""
    level: str = "low"
    threat_type: str = ""
    pid: int = 0
    description: str = ""
    action: str = "allow"
    timestamp: float = field(default_factory=time.time)


# ============================================================================
# 系统信息读取
# ============================================================================

def read_cpu() -> Dict:
    """读取 CPU 信息"""
    try:
        with open("/proc/stat") as f:
            line = f.readline()
        parts = line.split()
        user, nice, system, idle, iowait = map(int, parts[1:6])
        total = user + nice + system + idle + iowait
        used = total - idle - iowait
        usage = int(used * 100 / max(total, 1))
        return {"user": user, "system": system, "idle": idle,
                "iowait": iowait, "usage": usage}
    except:
        return {"usage": 0}


def read_memory() -> Dict:
    """读取内存信息"""
    try:
        mem = {}
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    mem["total"] = int(line.split()[1])
                elif line.startswith("MemAvailable:"):
                    mem["available"] = int(line.split()[1])
                elif line.startswith("SwapTotal:"):
                    mem["swap_total"] = int(line.split()[1])
                elif line.startswith("SwapFree:"):
                    mem["swap_free"] = int(line.split()[1])
        if "total" in mem and "available" in mem:
            mem["usage"] = int((mem["total"] - mem["available"]) * 100 /
                            max(mem["total"], 1))
        else:
            mem["usage"] = 0
        return mem
    except:
        return {"total": 0, "available": 0, "usage": 0}


def read_loadavg() -> Dict:
    """读取负载"""
    try:
        with open("/proc/loadavg") as f:
            parts = f.read().split()
        return {
            "load1": float(parts[0]),
            "load5": float(parts[1]),
            "load15": float(parts[2]),
        }
    except:
        return {"load1": 0, "load5": 0, "load15": 0}


def read_uptime() -> float:
    """读取运行时间"""
    try:
        with open("/proc/uptime") as f:
            return float(f.read().split()[0])
    except:
        return 0.0


def read_net() -> Dict:
    """读取网络统计"""
    try:
        rx, tx = 0, 0
        with open("/proc/net/dev") as f:
            for line in f:
                if ":" not in line:
                    continue
                name, data = line.strip().split(":", 1)
                name = name.strip()
                if name == "lo":
                    continue
                parts = data.split()
                rx += int(parts[0])
                tx += int(parts[8])
        return {"rx": rx, "tx": tx}
    except:
        return {"rx": 0, "tx": 0}


def get_telemetry() -> SystemTelemetry:
    """获取完整遥测"""
    cpu = read_cpu()
    mem = read_memory()
    load = read_loadavg()
    net = read_net()
    uptime = read_uptime()

    return SystemTelemetry(
        cpu_usage=cpu.get("usage", 0),
        mem_usage=mem.get("usage", 0),
        load1=load.get("load1", 0),
        load5=load.get("load5", 0),
        uptime_s=uptime,
        net_rx=net.get("rx", 0),
        net_tx=net.get("tx", 0),
        timestamp=time.time(),
    )


# ============================================================================
# AI 推理（直接 HTTP）
# ============================================================================

import socket
import ssl

class AIInference:
    """AI 推理调用"""

    @staticmethod
    def call(prompt: str,
            backend: str = "deepseek",
            model: str = "",
            api_key: str = "",
            timeout: int = 15) -> Dict:
        """
        调用 AI API，返回 {"text": ..., "latency_ms": ..., "error": ""}
        """
        if not api_key:
            api_key = (Config.DEEPSEEK_KEY if backend == "deepseek"
                      else Config.KIMI_KEY)

        if backend == "kimi":
            host, path = "api.moonshot.cn", "/v1/chat/completions"
            model = model or "moonshot-v1-8k"
        else:
            host, path = "api.deepseek.com", "/v1/chat/completions"
            model = model or "deepseek-chat"

        start = time.time()

        try:
            # 构造请求
            body = json.dumps({
                "model": model,
                "messages": [{"role": "user", "content": prompt}],
                "temperature": 0.3,
                "max_tokens": 512,
            }).encode()

            # HTTP 请求（简化版）
            request = (
                f"POST {path} HTTP/1.1\r\n"
                f"Host: {host}\r\n"
                f"Authorization: Bearer {api_key}\r\n"
                f"Content-Type: application/json\r\n"
                f"Content-Length: {len(body)}\r\n"
                f"User-Agent: AI-Linux-WS/1.0\r\n"
                f"Connection: close\r\n"
                f"\r\n"
            ).encode() + body

            # 建立 SSL 连接
            ctx = ssl.create_default_context()
        ctx.check_hostname = True
        ctx.verify_mode = ssl.CERT_REQUIRED
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(timeout)
            ssl_sock = ctx.wrap_socket(sock, server_hostname=host)
            ssl_sock.connect((host, 443))

            ssl_sock.sendall(request)

            # 接收响应
            chunks = []
            while True:
                try:
                    chunk = ssl_sock.recv(4096)
                    if not chunk:
                        break
                    chunks.append(chunk)
                except socket.timeout:
                    break
            ssl_sock.close()

            raw = b"".join(chunks).decode("utf-8", errors="replace")

            # 解析
            body_start = raw.find("\r\n\r\n")
            if body_start == -1:
                return {"text": "", "error": "无效响应", "latency_ms": 0}

            body = raw[body_start + 4:]

            try:
                resp = json.loads(body)
            except json.JSONDecodeError:
                # 尝试找 error 字段
                err_p = raw.find("\"error\":")
                if err_p != -1:
                    return {"text": "", "error": raw[err_p:err_p+100],
                           "latency_ms": 0}
                return {"text": "", "error": "JSON 解析失败",
                       "latency_ms": 0}

            choices = resp.get("choices", [])
            if not choices:
                return {"text": "", "error": "空响应",
                       "latency_ms": (time.time()-start)*1000}

            content = choices[0].get("message", {}).get("content", "")

            return {
                "text": content,
                "latency_ms": (time.time() - start) * 1000,
                "error": ""
            }

        except Exception as e:
            return {"text": "", "error": str(e),
                   "latency_ms": (time.time() - start) * 1000}


# ============================================================================
# WebSocket 客户端管理
# ============================================================================

class ClientManager:
    """管理所有 WebSocket 连接"""

    def __init__(self):
        self.clients: Dict[Any, Set] = {}
        self.subscriptions: Dict[Any, Set] = {}
        self.lock = threading.Lock()

    def register(self, client, subscriptions: Set[str]):
        with self.lock:
            self.clients[client] = subscriptions

    def unregister(self, client):
        with self.lock:
            self.clients.pop(client, None)

    def broadcast(self, event_type: str, data: Dict):
        """向所有订阅了此事件的客户端广播"""
        with self.lock:
            for client, subs in list(self.clients.items()):
                if event_type in subs:
                    try:
                        asyncio.run(client.send(json.dumps(data)))
                    except Exception:
                        pass


# ============================================================================
# WebSocket 处理（asyncio）
# ============================================================================

if HAS_WEBSOCKETS:

    client_mgr = ClientManager()
    ai = AIInference()

    async def handle_client(websocket, path):
        """处理单个客户端连接"""
        client = websocket
        subscribed: Set[str] = {"telemetry", "chat_response"}

        try:
            # 等待第一条消息（订阅）
            init = await asyncio.wait_for(client.recv(), timeout=10)
            msg = json.loads(init)

            if msg.get("type") == "subscribe":
                subscribed = set(msg.get("events", []))
                if not subscribed:
                    subscribed = {"telemetry", "chat_response"}

            client_mgr.register(client, subscribed)

            # 发送欢迎消息
            await client.send(json.dumps({
                "type": "connected",
                "timestamp": time.time(),
                "message": "AI Linux WebSocket 已连接",
                "subscribed": list(subscribed),
            }))

            logger.info(f"客户端连接，订阅: {subscribed}")

            # 主循环
            async for message in websocket:
                try:
                    msg = json.loads(message)
                    await process_message(client, msg, subscribed)
                except json.JSONDecodeError:
                    await client.send(json.dumps({
                        "type": "error",
                        "error": "无效的 JSON",
                    }))
                except Exception as e:
                    logger.error(f"处理消息出错: {e}")
                    await client.send(json.dumps({
                        "type": "error",
                        "error": str(e),
                    }))

        except asyncio.TimeoutError:
            logger.warning("客户端连接超时")
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            client_mgr.unregister(client)
            logger.info("客户端断开连接")

    async def process_message(client, msg: Dict, subscribed: Set):
        """处理客户端消息"""
        msg_type = msg.get("type", "")

        if msg_type == "subscribe":
            # 重新订阅
            subscribed.clear()
            subscribed.update(msg.get("events", []))
            client_mgr.register(client, subscribed)
            await client.send(json.dumps({
                "type": "subscribed",
                "events": list(subscribed),
            }))

        elif msg_type == "chat":
            # AI 对话
            prompt = msg.get("prompt", "")
            if not prompt:
                await client.send(json.dumps({
                    "type": "error",
                    "error": "prompt 不能为空",
                }))
                return

            # 获取后端
            backend = msg.get("backend", Config.DEFAULT_BACKEND)

            # 调用 AI
            result = ai.call(prompt, backend=backend)

            response = {
                "type": "chat_response",
                "prompt": prompt,
                "backend": backend,
                "timestamp": time.time(),
                "latency_ms": result["latency_ms"],
            }

            if result.get("error"):
                response["error"] = result["error"]
                response["text"] = ""
            else:
                response["text"] = result["text"]

            await client.send(json.dumps(response))

            # 也推送给订阅了 chat_response 的所有客户端
            client_mgr.broadcast("chat_response", response)

        elif msg_type == "inference_request":
            # 推理请求
            prompt = msg.get("prompt", "")
            domain = msg.get("domain", "system")
            req_id = msg.get("id", hashlib.md5(f"{prompt}{time.time()}".encode()).hexdigest()[:16])
            backend = msg.get("backend", Config.DEFAULT_BACKEND)

            result = ai.call(prompt, backend=backend)

            response = {
                "type": "inference_response",
                "id": req_id,
                "prompt": prompt,
                "domain": domain,
                "backend": backend,
                "timestamp": time.time(),
                "latency_ms": result["latency_ms"],
                "text": result.get("text", ""),
                "error": result.get("error", ""),
            }

            await client.send(json.dumps(response))

        elif msg_type == "ping":
            await client.send(json.dumps({
                "type": "pong",
                "timestamp": time.time(),
            }))

        else:
            await client.send(json.dumps({
                "type": "error",
                "error": f"未知的消息类型: {msg_type}",
            }))


# ============================================================================
# 定时任务
# ============================================================================

def telemetry_loop():
    """定时推送遥测数据（后台线程）"""
    while True:
        try:
            tel = get_telemetry()
            data = {
                "type": "telemetry",
                **asdict(tel),
            }
            # 使用 asyncio 在新事件循环中广播
            try:
                loop = asyncio.new_event_loop()
                asyncio.set_event_loop(loop)
                loop.run_until_complete(
                    client_mgr.broadcast("telemetry", data)
                )
                loop.close()
            except Exception:
                pass

        except Exception as e:
            logger.error(f"遥测推送出错: {e}")

        time.sleep(Config.TELEMETRY_INTERVAL)


# ============================================================================
# 主入口
# ============================================================================

def main():
    print()
    print("  ╔═══════════════════════════════════════════════╗")
    print("  ║   AI Linux WebSocket Server              ║")
    print("  ║   实时推理推送 + 遥测 + 决策事件         ║")
    print("  ╚═══════════════════════════════════════════════╝")
    print()

    if not HAS_WEBSOCKETS:
        print("错误: 需要安装 websockets")
        print("  pip install websockets")
        return

    if not Config.DEEPSEEK_KEY and not Config.KIMI_KEY:
        print("⚠️  警告: 未设置 API 密钥，环境变量:")
        print("  export DEEPSEEK_API_KEY=xxx")
        print("  export KIMI_API_KEY=xxx")
        print()

    print(f"  监听: ws://{Config.HOST}:{Config.PORT}")
    print(f"  后端: {'DeepSeek' if Config.DEEPSEEK_KEY else 'Kimi' if Config.KIMI_KEY else '未配置'}")
    print(f"  遥测间隔: {Config.TELEMETRY_INTERVAL}s")
    print()

    # 启动遥测线程
    telemetry_thread = threading.Thread(target=telemetry_loop, daemon=True)
    telemetry_thread.start()

    # 启动 WebSocket 服务器
    logger.info(f"WebSocket 服务启动 ws://{Config.HOST}:{Config.PORT}")

    try:
        asyncio.run(
            websockets.serve(
                handle_client,
                Config.HOST,
                Config.PORT,
                ping_interval=30,
                ping_timeout=10,
            )
        )
    except KeyboardInterrupt:
        print("\n  正在关闭...")
    except OSError as e:
        if e.errno == 98:
            print(f"错误: 端口 {Config.PORT} 已被占用")
            print("  请使用 --port 指定其他端口")
        else:
            raise


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="AI Linux WebSocket Server")
    parser.add_argument("--host", default=Config.HOST, help="监听地址")
    parser.add_argument("--port", type=int, default=Config.PORT, help="监听端口")
    args = parser.parse_args()

    Config.HOST = args.host
    Config.PORT = args.port

    main()
