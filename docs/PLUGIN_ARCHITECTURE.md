# KAI Linux — 全插件热插拔架构（Everything-as-a-Plugin）

## 设计原则

> **KAI Linux 的一切都是插件。** 内核模块、推理后端、Skill、MCP 工具、
> 功能引擎、数据采集器、通知渠道、网关 API、UI 界面、硬件后端——
> 每一类都是统一生命周期管理的插件，支持运行时热插拔。

```
                        ┌─────────────────────────┐
                        │   Plugin Master (主控)   │
                        │   统一注册中心 + 事件总线  │
                        └────────────┬────────────┘
                                     │ 统一生命周期
      ┌───────────┬───────────┬──────┴─────┬───────────┬──────────┐
      ▼           ▼           ▼            ▼           ▼          ▼
┌─────────┐ ┌─────────┐ ┌─────────┐  ┌─────────┐ ┌─────────┐ ┌─────────┐
│ 内核模块 │ │ 推理后端 │ │ Skill  │  │ MCP工具 │ │功能引擎 │ │硬件后端 │
├─────────┤ ├─────────┤ ├─────────┤  ├─────────┤ ├─────────┤ ├─────────┤
│ 数据采集 │ │ 通知渠道 │ │ 网关API │  │ UI 界面 │ │(更多...)│ │         │
└─────────┘ └─────────┘ └─────────┘  └─────────┘ └─────────┘ └─────────┘
      ▲            ▲           ▲            ▲           ▲          ▲
      └────────────┴───────────┴────────────┴───────────┴──────────┘
                   全部通过 PluginRegistry 热插拔
```

## 统一插件类型（11 类，覆盖所有模块）

| Kind | 说明 | 现有模块 | 原类型 |
|---|---|---|---|
| `kernel` | 内核模块 | ai_core, kai_cache, kai_infer, kai_syscall, kai_resilience, kai_metrics, kai_net_opt, kai_mem_opt, kai_tracing | PLUGIN_TYPE_* |
| `backend` | 推理后端 | deepseek, kimi, local_rule, ensemble | PLUGIN_TYPE_BACKEND |
| `skill` | 能力 | scheduling, io, security, memory, code, orchestrate | SKILL_CAT_* |
| `mcptool` | MCP 工具 | sched_analyze, io_inspect, security_scan… | mcp_tool |
| `feature` | 功能引擎 | cache, load_balancer, multi_model, session, tracing | — |
| `collector` | 数据采集 | cpu, mem, net, disk, proc, ai_stats | — |
| `notifier` | 通知渠道 | webhook, dingtalk, slack, cloud | — |
| `gwapi` | 网关 API | rest(8080), monitor(9090), ws(9998), netlink | — |
| `ui` | 界面 | cli, tui, web, monitor | — |
| `hwaccel` | 硬件后端 | nvidia, amd, intel, ascend, riscv | — |
| `sec` | 安全 | crypto, tls, rbac | — |

## 统一生命周期

所有插件（无论内核态/用户态）都遵循同一状态机：

```
                ┌──────────────────────────────────────┐
                │                                      │
register ──▶ LOADED ──enable──▶ ENABLED ──start──▶ RUNNING
   │              ▲                │                   │
   │              │                ▼                   ▼
   └── unregister  └── unload ◀── disable ◀── stop ── (healthcheck ↓)
                                                         │ 失败
                                                         ▼
                                                      DEGRADED
                                                         │ recover
                                                         ▼
                                                       ENABLED
```

状态：`REGISTERED → LOADED → ENABLED → RUNNING → DISABLED → UNLOADED → (注销)`

每个状态迁移发出事件，广播给所有插件（事件总线）。

## 热插拔语义

| 操作 | 用户态 | 内核态 |
|---|---|---|
| **加载** | 动态 import + 注册 | `insmod` |
| **启用** | `plugin.enable()` | `echo enable > /proc/ai_plugins/list` |
| **运行** | 启动工作线程 | 激活推理路径 |
| **禁用** | `plugin.disable()` 保留内存 | 等待活跃推理，停用 |
| **停止** | 停止线程 | `rmmod` |
| **重载** | re-import（A/B 版本）| 重新加载 .ko |
| **热升级** | 原子切换版本 | 模型热更新 `kai_model_hot_update` |

## 依赖解析

插件声明依赖，主控按拓扑序加载：

```json
{"plugin": "ensemble", "depends": ["deepseek", "kimi"]}
```

## 事件总线

热插拔事件广播给所有插件，用于联动（如后端下线 → 负载均衡摘除）：

```
plugin.registered / unregistered
plugin.loaded / unloaded
plugin.enabled / disabled
plugin.started / stopped
plugin.degraded / recovered
plugin.config_changed
```

## 主控 API（示例）

```bash
kai-plugin list                      # 所有插件状态
kai-plugin install  --kind backend --spec deepseek.json
kai-plugin uninstall deepseek
kai-plugin enable  kai_infer
kai-plugin disable kai_infer
kai-plugin reload  gateway
kai-plugin start   web / stop web
kai-plugin watch   / --watch         # 观察热插拔事件
kai-plugin status <name>
kai-plugin export  > snapshot.json   # 导出状态（恢复用）
kai-plugin import  < snapshot.json
```

## 一致性保障

- 内核插件：引用计数 + 活跃推理等待（`ai_plugin_mgr.c` 已实现）
- 依赖未满足时拒绝加载
- 状态迁移幂等（重复 enable 无害）
- 插件崩溃自动重启（watchdog）

## 关键实现

1. `plugins/plugin_master.py` — 用户态统一主控（全插件注册中心）
2. `plugins/core/ai_plugin_mgr.c` — 内核态插件管理器（已支持热插拔）
3. `kai-plugin` — 热插拔管理 CLI
