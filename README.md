<div align="center">

# ⚡ KAI Linux — 内核级人工智能操作系统

### *把 AI 推理嵌入 Linux 内核 —— 内核即智能*

```
K · Kernel     A · AI     I · Intelligence
```

![Kernel](https://img.shields.io/badge/Kernel-%E2%89%A5%206.6%20(LTS%206.12)-00d4ff?style=flat-square&labelColor=0d0d20&color=111130)
![Arch](https://img.shields.io/badge/Arch-x86__64%20%C2%B7%20arm64%20%C2%B7%20riscv64-3ef0a0?style=flat-square&labelColor=0d0d20)
![Lang](https://img.shields.io/badge/C%20%C2%B7%20Python%20%C2%B7%20eBPF-111130?style=flat-square&labelColor=0d0d20&color=a78bfa)
![sys_infer](https://img.shields.io/badge/核心系统调用-sys__infer__()-f472b6?style=flat-square&labelColor=0d0d20)
![AI_Linux](https://img.shields.io/badge/AI_Linux-Repo%20on%20GitHub-0d0d20?style=flat-square&logo=github&logoColor=fff)

[![CI](https://img.shields.io/github/actions/workflow/status/Cheng12443/AI_Linux/ci.yml?branch=main&label=CI&style=flat-square&labelColor=0d0d20&color=3ef0a0&logo=githubactions&logoColor=fff)](https://github.com/Cheng12443/AI_Linux/actions/workflows/ci.yml)
![Security](https://img.shields.io/badge/Security%20Scan-C%3A0%20H%3A0%20M%3A0%20L%3A0-00d4ff?style=flat-square&labelColor=0d0d20)
![Stress](https://img.shields.io/badge/Stress%20Test-100%25%20success%20%C2%B7%201.7M%20ops-a78bfa?style=flat-square&labelColor=0d0d20)
![Plugins](https://img.shields.io/badge/Everything--as--a--Plugin-38%20hot--pluggable-fbbf24?style=flat-square&labelColor=0d0d20)

**✨ 交互式官网页面（GitHub Pages）** → [**KAI Linux 完整介绍**](https://cheng12443.github.io/AI_Linux/)

</div>

---

## 🧠 它是什么

AI Linux（KAI Linux）是一个 **把 AI 推理能力嵌入 Linux 内核** 的开源操作系统项目 —— 让调度器、内存、网络、安全模块直接用模型做决策，而不只是"内核 + 用户态 AI 框架"的堆叠。

- 🤖 **AI 调度器** — 模型参与 CFS / EEVDF 调度决策
- 🚀 **AI IO** — 网卡 / 存储路径就地异常检测
- 🧠 **AI 内存** — kswapd 用模型预测页面热度
- 🛡 **AI 安全** — 内核态异常行为实时检测
- ⚙️ **`sys_infer()`** — 推理成为第一类系统调用

> 愿景：未来的内核不只是资源的调度者，更是 **能感知、能推断、能自我优化** 的智能底座。

## 🌐 交互式落地页

仓库内置一页式全站介绍（暗色科技风，含架构分层、能力模块、子产品与路线图）：

| 入口 | 链接 |
|---|---|
| 🌍 GitHub Pages（推荐） | https://cheng12443.github.io/AI_Linux/ |
| 📄 源文件 | [`docs/KAI-Linux.html`](docs/KAI-Linux.html) |

## 🏗️ 架构分层

```
┌─────────────────────────────────────────────────────────┐
│  用户空间  apps · KAI Shell (TUI) · KAI Studio (Web)     │
├─────────────────────────────────────────────────────────┤
│  Layer2  KAI Gateway · MCP 协议 · Skills 技能路由        │
├─────────────────────────────────────────────────────────┤
│   AI 推理服务 · eBPF 探针 · 决策引擎 · 模型管理          │
│        ▲                                    ▲           │
│        │ netlink / io_uring / sys_infer     │           │
├────────┴────────────────────────────────────┴───────────┤
│  内核空间                                              │
│   ├─ ai_core/         AI 核心子系统（推理/缓存/决策）   │
│   ├─ sched_ext/       AI 调度器插件                    │
│   ├─ ebpf/            内核态 eBPF 推理程序              │
│   ├─ drivers/ai/      AI 加速器驱动抽象                 │
│   └─ sys_infer        推理系统调用                      │
└─────────────────────────────────────────────────────────┘
```

| 模块 | 说明 |
|---|---|
| `ai_core` | AI 核心：同步/异步推理接口、模型缓存、决策引擎 |
| `sched_ext` | 基于 sched_ext 的 AI 调度器 |
| `ebpf` | 内核 eBPF 推理 / 探针程序 |
| `layer2` | KAI Gateway：MCP 协议 · 技能路由 · 工具链 |
| `kai/*` | 云端/多机/监控/安全/韧性/网络/调度等领域扩展 |
| `plugins/` | KAI Forge 插件系统（AI Plugin SDK） |
| `ui` | KAI Shell / KAI Watch 等交互界面 |

## 🚀 快速开始

```bash
# 1. 编译 AI 核心模块
cd kbuild
make ai_core

# 2. 编译 eBPF 程序
cd ../ebpf
make

# 3. 跑测试
cd ../tests
python3 test_ai_core.py

# 4. 注入内核（root，6.6+）
insmod ai_core.ko
dmesg | tail
```

## 🧰 环境要求

- **内核**：≥ 6.6（推荐 6.12 LTS）
- **架构**：x86_64 / arm64 / riscv64
- **工具链**：GCC ≥ 9 / Clang ≥ 14 / bpftool
- **eBPF**：libbpf ≥ 1.0

## 📚 文档

| 文档 | 说明 |
|---|---|
| [📖 架构设计](docs/ARCHITECTURE.md) | 总体架构与模块边界 |
| [🧩 Layer2 架构](docs/LAYER2_ARCHITECTURE.md) | MCP 网关层详解 |
| [🔌 插件架构](docs/PLUGIN_ARCHITECTURE.md) | KAI Forge 插件系统 |
| [🛠 API 参考](docs/API.md) | 对外接口 |
| [📦 部署指南](docs/DEPLOYMENT.md) | 安装与部署 |
| [🧯 故障排查](docs/TROUBLESHOOTING.md) | 常见问题 |
| [🗺 路线图](ROADMAP.md) | 性能/功能/可靠性优化计划 |

## 🧩 KAI 子产品族

```
KAI Core    · 内核 AI 核心（ai_core.ko）      KAI Gateway · Layer2 MCP 网关
KAI Shell   · TUI 交互界面                     KAI Studio  · Web Dashboard
KAI SDK     · Python 开发套件                  KAI Hub     · WebSocket 推送
KAI Watch   · 性能监控面板                     KAI Forge   · 插件系统
```

---

<div align="center">

**KAI Linux** · Kernel-level AI Operating System

如果你觉得这个方向有意思 —— ⭐ Star · 🍴 Fork · 🛠 提 PR 都欢迎

</div>
