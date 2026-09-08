# Layer2 AI 网关架构设计

## 整体架构

```
┌──────────────────────────────────────────────────────────────────────────┐
│                           用户空间 (User Space)                          │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────┐    │
│  │                    Layer2 AI 网关 (ai_layer2_gateway)            │    │
│  │                                                                   │    │
│  │  ┌──────────────┐   ┌───────────────┐   ┌────────────────┐      │    │
│  │  │ netlink 接收 │──▶│ 决策分发队列  │──▶│ API 推理引擎   │      │    │
│  │  │    模块      │   │  (async)      │   │ OpenAI/Claude/ │      │    │
│  │  │              │   │               │   │ Ollama/Soft    │      │    │
│  │  └──────────────┘   └───────────────┘   └───────┬────────┘      │    │
│  │                                                  │               │    │
│  │  ┌──────────────┐   ┌───────────────┐   ┌──────┴────────┐      │    │
│  │  │   提示词工程  │   │   决策缓存    │   │  响应解析器   │      │    │
│  │  │  (Prompts)   │   │  (TTL 5s)     │   │  (JSON Parser)│      │    │
│  │  └──────────────┘   └───────────────┘   └───────────────┘      │    │
│  │                                                                   │    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                    │                                    │
│                          netlink / io_uring / shmem                    │
│                                    │                                    │
└────────────────────────────────────┼────────────────────────────────────┘
                                     │
                     ┌───────────────┴───────────────┐
                     │        内核空间 (Kernel)       │
                     │                               │
                     │  ┌─────────────────────────┐  │
                     │  │    AI Core (Layer1)     │  │
                     │  │                         │  │
                     │  │  ┌─────────────────┐   │  │
                     │  │  │  模型注册/推理  │   │  │
                     │  │  │  sys_infer()   │   │  │
                     │  │  └────────┬────────┘   │  │
                     │  │           │            │  │
                     │  │  ┌────────▼────────┐   │  │
                     │  │  │  决策仲裁器     │   │  │
                     │  │  │  Layer1 优先    │   │  │
                     │  │  │  Layer2 复核    │   │  │
                     │  │  └────────┬────────┘   │  │
                     │  │           │            │  │
                     │  └───────────┼────────────┘  │
                     │              │               │
                     │  ┌───────────┼───────────┐   │
                     │  │           │           │   │
                     │  ▼           ▼           ▼   │
                     │ ┌──────┐  ┌──────┐  ┌──────┐│
                     │ │sched_│  │ XDP   │  │ BPF  ││
                     │ │ext   │  │ BPF   │  │ LSM  ││
                     │ │AI    │  │ AI    │  │ AI   ││
                     │ │调度器│  │ 网络  │  │ 安全 ││
                     │ └──┬───┘  └───┬───┘  └──┬───┘│
                     │    │          │          │   │
                     │    ▼          ▼          ▼   │
                     │  ┌─────────────────────────┐ │
                     │  │  ai_memory AI 内存管理  │ │
                     │  │  (kswapd 页面热度预测)  │ │
                     │  └─────────────────────────┘ │
                     └───────────────────────────────┘
```

## 双向信息路由流程

### 流程1：异步复核模式（推荐，mode=async）

```
应用/系统调用
     │
     ▼
ai_infer_sched_decision()  ── Layer1 推理 ──▶ AI_KEEP
     │                                          │
     │ 决策置信度 < 阈值                         │
     ▼                                          ▼
ai_layer2_request_decision()              应用决策（使用 Layer1）
     │                                          │
     │ netlink 发送请求                          │
     ▼                                          │
Layer2 网关接收                              
     │
     │ 检查决策缓存（TTL=5s）
     │ 命中 ──▶ 直接返回缓存结果
     │ 未命中 ──▶ 发送 API 请求
     │
     ▼
外部 AI API（OpenAI/Claude/Ollama）
     │
     │ HTTP POST /v1/chat/completions
     ▼
解析 JSON 响应
     │
     ▼
netlink 发送决策结果 ──▶ 内核决策仲裁器
                              │
                              │ Layer2 置信度 > 阈值
                              ▼
                         更新调度策略
                         记录统计信息
```

### 流程2：同步覆盖模式（mode=override）

```
应用请求
     │
     ▼
ai_layer2_request_decision() ── 阻塞等待 Layer2
     │ (timeout=1s)
     │
     ▼
Layer2 API 推理
     │
     │ Layer2 置信度 > 阈值
     ▼
Layer2 决策 ── 覆盖 Layer1 决策
```

### 流程3：编排联合决策（mode=orchestra）

```
应用请求
     │
     ├──────────────────────┐
     ▼                      ▼
Layer1 推理           Layer2 API 推理
(内核，快速)          (用户态，精确)
     │                      │
     ▼                      ▼
score_L1 + weight1   score_L2 + weight2
     │                      │
     └────────┬─────────────┘
              ▼
        加权融合决策
        = w1*score_L1 + w2*score_L2
              │
              ▼
         最终调度决策
```

## 数据流：netlink 消息格式

### 内核 → 用户态（决策请求）

```
nlmsg_type: AI_L2_CMD_DECISION_REQ
nlmsg_len:  NLMSGHDR + sizeof(sched_context)
nlmsg_seq:  request_id (唯一标识)

Payload (sched_context):
  timestamp_ns:    1588845221000000000
  pid:             12345
  cpu_util:        768
  nvcsw:           25
  nivcsw:          5
  layer1_decision: AI_KEEP
  layer1_score:    5500
  layer1_latency_ns: 12300
```

### 用户态 → 内核（决策响应）

```
nlmsg_type: AI_L2_CMD_DECISION_RES
nlmsg_len:  NLMSGHDR + sizeof(decision)
nlmsg_seq:  request_id (回传)

Payload (decision):
  request_id:       1588845221000000001
  domain:           AI_L2_DOMAIN_SCHED
  decision:         AI_DECISION_PROMOTE
  confidence:       85
  override_layer1: 0
  reason:           "CPU利用率高且I/O等待短，建议升权"
  model_name:       "gpt-4o-mini"
  model_provider:   "openai"
  model_latency_ns: 45000000
```

## 共享内存协议

用于大批量数据（遥测历史、模型权重）：

```
┌──────────────────────────────────────────┐
│  layer2_shmem (4MB)                      │
│                                          │
│  ┌────────────────────────────────────┐ │
│  │ Header (64B)                        │ │
│  │   magic = 0x41494C32               │ │
│  │   version = 0x00010000             │ │
│  │   size = 4MB                       │ │
│  │   producer_idx (atomic)            │ │
│  │   consumer_idx (atomic)            │ │
│  └────────────────────────────────────┘ │
│                                          │
│  ┌────────┬────────┬────────┬────────┐   │
│  │ Slot 0 │ Slot 1 │ Slot 2 │ Slot N │   │
│  │ 64KB   │ 64KB   │ 64KB   │ 64KB   │   │
│  │ Telemetry │ Sched  │ IO     │ ...    │   │
│  └────────┴────────┴────────┴────────┘   │
│                                          │
└──────────────────────────────────────────┘

mmap() → /proc/ai_layer2/shmem
```

## 提示词工程

### 调度场景

```
系统提示：
你是 Linux 内核 AI 调度助手。你的职责是根据系统实时状态，
给出最优的 CPU 调度决策。

上下文注入（系统遥测）：
- CPU 利用率: {cpu_util}%
- 上下文切换率: {ctx_switch_rate}/s
- 负载均值: {loadavg_1}
- 内存压力: {mem_available}MB
- Layer1 AI 置信度: {layer1_confidence}%

上下文注入（任务特征）：
- PID: {pid}
- CPU 运行时间: {sum_exec_runtime}ns
- 自愿切换: {nvcsw}
- 非自愿切换: {nivcsw}

决策要求：
- 返回 JSON 格式
- decision: promote/demote/migrate/batch/keep
- confidence: 0-100
- reason: 中文理由
```

## 错误处理与降级

| 错误场景 | 处理策略 |
|---|---|
| API 超时 | 使用缓存或回退 Layer1 |
| API 认证失败 | 记录错误，继续 Layer1 |
| 连续 3 次 API 失败 | 启用降级模式，仅用 Layer1 |
| Layer2 置信度过低 | 采纳 Layer1 决策 |
| netlink 连接断开 | 自动重连，队列积压时丢弃旧请求 |
| 共享内存满 | 丢弃最旧条目（环形缓冲） |

## 性能基准

| 路径 | 延迟 | 吞吐量 |
|---|---|---|
| Layer1 推理（内核同步） | ~10-100μs | ~1M req/s |
| Layer2 推理（本地 Ollama） | ~50-200ms | ~100 req/s |
| Layer2 推理（OpenAI API） | ~200-2000ms | ~10 req/s |
| netlink 往返（内核↔用户态） | ~5-20μs | ~500K msg/s |
| 决策缓存命中 | ~0μs | ∞ |
