# 把 AI 放进内核 —— 落点与验收

> 目标不是"跑 AI 的系统"，而是**推理决策发生在内核执行路径内**。
> 本文把每个"AI 决策点"钉到具体内核钩子：数据从哪来、怎么推理、决策回写哪、怎么验收。

## 总体回路

```
        内核执行路径（每次发生都会问 AI）
        ┌──────────────────────────────────────────────┐
        │                                              │
 进程调度 │  任务切换 → 特征(util/ctx-switch) → 决策→调度 │  sched_ext
 网卡收包 │  包到达  → 特征(IP/port/flags)   → 决策→DROP │  XDP
 进程 exec│  可执行文件→ 特征(comm/argv)     → 决策→放行 │  BPF LSM
 内存换页 │  页面访问 → 频率/时间序列        → 预测→预取 │  vmscan 钩子
        │                    │                          │
        └────────────────────┼──────────────────────────┘
                             ▼
                   sys_infer() #548  (vmlinux)
                             │  kai_engine 注册表（热插拔）
                    ┌────────┴─────────────┐
                    ▼                       ▼
              ai_core 引擎              Layer2 引擎
         (缓存/量化/规则, 内核态)    (engine_ai_core → DeepSeek/Kimi)
```

## 落点清单（每一处都是"推理在内核态发生"）

| # | 位置 | 内核钩子 | 输入特征 | 推理输出→回写 | 激活 | 验收 |
|---|---|---|---|---|---|---|
| 1 | **sys_infer 系统调用** | `syscall_64.tbl #548` | 任意进程输入 | 推理结果→用户态 | `integrate_sys_infer.sh` + 编内核 | `test_sys_infer` 返回 score |
| 2 | **AI 调度** | `sched_ext`（`kai_sched_ext.bpf.c`） | task: util/nvcsw/nivcsw/prio/numa | PROMOTE/DEMOTE/MIGRATE→scx 选择 CPU | 内核 6.12 + 加载 BPF 调度器 | ringbuf 事件 + 任务实际被迁移 |
| 3 | **网络 AI 分类** | `XDP`（`ai_xdp.bpf.c`） | 包: IP/端口/TCPflags/长度 | score→DROP/PASS/REDIRECT | `ip link set xdp obj …` | `xdp_drop` 计数增长 |
| 4 | **安全 AI** | `BPF LSM task_exec/file_open`（`ai_lsm.bpf.c`） | exec: comm/argv/频率 | allow/block | 加载 LSM BPF 程序 | 恶意样本被拒 EPERM |
| 5 | **内存热度 AI** | `kai_mem_opt.c`（vmscan 侧钩子） | 页面访问序列 | 热点页→预取标记 | insmod kai_mem_opt | `/proc/ai_memory` 有预测 |
| 6 | **内核问大模型** | `engine_ai_core.c` 引擎 | 内核侧特征 | → ai_core → Layer2(DeepSeek/Kimi) | insmod ai_core + engine_ai_core | syscall 结果含外部模型回复 |
| 7 | **后端热插拔** | `kai_engine_register/unregister` | — | 引擎可换（demo/ai_core/Layer2） | 任意引擎 .ko | register 日志 |

## 关键设计约束

- **syscall 常驻内核**（编进 vmlinux），引擎热插拔 → 每次推理都经内核统一：鉴权→缓存→引擎→审计
- **eBPF 决策零拷贝**：特征在钩子现场提取，BPF 内直接算分，不切用户态
- **Layer2 是增强不是必需**：无网/无 key 时内核仍能决策（本地量化/规则引擎兜底）

## 端到端验收脚本

```bash
sudo bash tools/verify_ai_in_kernel.sh          # 见同目录：分阶段 PASS/FAIL
```

## 里程碑判定

- ☐ [M1] 内核含 `sys_kai_infer` 符号（grep /proc/kallsyms）
- ☐ [M2] `test_sys_infer` 输出 score（推理经内核完成）
- ☐ [M3] 换 Layer2 引擎后同 syscall 返回模型文本（内核能问外部 AI）
- ☐ [M4] sched_ext 调度事件含 AI 决策（ringbuf 采样）
- ☐ [M5] XDP 挂载后命中 drop（AI 在网卡层拦了包）
- ☐ [M6] LSM 拒绝被判定恶意 exec
