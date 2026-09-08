# KAI Linux — API 文档

## 目录

1. [内核系统调用](#内核系统调用)
2. [Python SDK](#python-sdk)
3. [Web API](#web-api)
4. [WebSocket](#websocket)
5. [MCP 工具](#mcp-工具)
6. [配置接口](#配置接口)

---

## 内核系统调用

### sys_infer()

AI 推理系统调用，系统调用号 `548`。

```c
SYSCALL_DEFINE1(kai_infer, struct kai_infer_args __user *, args_ptr)
```

**参数结构** `struct kai_infer_args`：

| 字段 | 类型 | 说明 |
|---|---|---|
| model_name_ptr | u64 | 模型名称指针 |
| input_ptr | u64 | 输入缓冲区指针 |
| input_size | u64 | 输入大小 |
| output_ptr | u64 | 输出缓冲区指针 |
| output_size | u64 | 输出缓冲区大小 |
| flags | u64 | 标志位 |
| request_id | u64 | 请求 ID（输出）|
| latency_ns | u64 | 延迟（输出）|

**返回值**：≥0 输出字节数，<0 错误码

**权限**：需要 `CAP_SYS_ADMIN` 或 `CAP_SYS_NICE`

---

## Python SDK

### AIClient

```python
from ai_linux import AIClient

ai = AIClient(backend="deepseek")  # 或 "kimi"
```

#### 方法

| 方法 | 说明 |
|---|---|
| `ask(prompt, system=None)` | 发送问题 |
| `sched_analyze(pid=None)` | 调度分析 |
| `net_analyze(connections=False)` | 网络分析 |
| `security_scan(pid=None, full=False)` | 安全检测 |
| `memory_analyze(full=False)` | 内存分析 |
| `code_generate(requirement, language)` | 代码生成 |
| `orchestrate(requirement)` | 系统编排 |
| `clear_history()` | 清空历史 |
| `ws_connect()` | 连接 WebSocket |
| `ws_subscribe(event, callback)` | 订阅事件 |

#### 示例

```python
from ai_linux import AIClient

ai = AIClient()
response = ai.ask("分析系统负载")
print(response.text)
print(response.backend, response.latency_ms)
```

---

## Web API

### GET /api/status

获取系统状态。

```bash
curl http://localhost:8080/api/status
```

响应：
```json
{
  "cpu": {"usage": 45},
  "memory": {"usage": 60, "total": 32768, "available": 13107},
  "loadavg": {"load1": 0.5, "load5": 0.4, "load15": 0.3},
  "uptime": "86400",
  "backend": "deepseek"
}
```

### POST /api/ask

发送 AI 请求。

```bash
curl -X POST http://localhost:8080/api/ask \
  -H "Content-Type: application/json" \
  -d '{"prompt": "分析调度"}'
```

### GET /api/metrics

获取监控指标。

```bash
curl http://localhost:9090/api/metrics
```

---

## WebSocket

连接：`ws://localhost:9998/`

### 订阅

```javascript
const ws = new WebSocket("ws://localhost:9998/");
ws.send(JSON.stringify({
  type: "subscribe",
  events: ["telemetry", "chat_response", "sched_decision"]
}));
```

### 消息类型

| 类型 | 方向 | 说明 |
|---|---|---|
| `chat` | 客户端→服务器 | 发送对话 |
| `chat_response` | 服务器→客户端 | 对话响应 |
| `inference_request` | 客户端→服务器 | 推理请求 |
| `inference_response` | 服务器→客户端 | 推理响应 |
| `telemetry` | 服务器→客户端 | 系统遥测 |
| `sched_decision` | 服务器→客户端 | 调度决策 |
| `ping` / `pong` | 双向 | 心跳 |

---

## MCP 工具

### tools/list

```json
{"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}
```

### tools/call

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/call",
  "params": {
    "name": "sched_analyze",
    "arguments": {"pid": 1234}
  }
}
```

### 可用工具

| 工具 | 说明 |
|---|---|
| `sched_analyze` | 分析进程调度 |
| `io_inspect` | 检查网络流量 |
| `security_scan` | 检测恶意行为 |
| `memory_predict` | 预测页面热度 |
| `sys_info` | 获取系统信息 |
| `netstat_query` | 查询连接 |
| `disk_io_analyze` | 分析磁盘 |
| `process_kill` | 终止进程 |
| `network_block` | 阻断 IP |
| `cpu_pin` | 绑定 CPU |

---

## 配置接口

### 配置结构（layer2.yaml）

```yaml
backend:
  provider: deepseek      # deepseek | kimi
  key_env: DEEPSEEK_API_KEY

model:
  name: deepseek-chat     # 或 moonshot-v1-8k

mode:
  type: async             # async | sync | override

routing:
  thresholds:
    scheduling: 7000
    io: 7000
    security: 8000
```

### 环境变量

| 变量 | 说明 |
|---|---|
| `DEEPSEEK_API_KEY` | DeepSeek 密钥 |
| `KIMI_API_KEY` | Kimi K3 密钥 |
| `KAI_BACKEND` | 后端选择 |
| `KAI_MODE` | 运行模式 |
| `AI_THRESHOLD_SCHED` | 调度阈值 |
| `AI_THRESHOLD_IO` | IO 阈值 |
| `AI_THRESHOLD_SEC` | 安全阈值 |
| `AI_LOG_LEVEL` | 日志级别 |
