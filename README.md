# AI Linux — 内核级人工智能操作系统

## 目标

把 AI 推理能力嵌入 Linux 内核，实现：
- **AI 调度器**：模型参与 CFS/EEVDF 调度决策
- **AI IO**：网卡/存储路径上就地做异常检测
- **AI 内存**：kswapd 用模型预测页面热度
- **AI 安全**：内核态异常行为检测
- **sys_infer()**：推理作为第一类系统调用

## 架构

```
用户空间
 ├── AI 推理服务（Python/C++，模型管理）
 ├── eBPF 探针（内核数据采集）
 └── 应用（调度建议、IO决策、安全检测）
      ↑
      ↓（netlink / io_uring / sys_infer）
内核空间
 ├── ai_core/          AI 核心子系统
 │   ├── 推理接口（同步/异步）
 │   ├── 模型缓存
 │   └── 决策引擎
 ├── sched_ext/        AI 调度器插件
 ├── ebpf/             eBPF 推理程序
 ├── drivers/ai/       AI 加速器驱动抽象
 └── sys_infer         推理系统调用
```

## 版本要求

- 内核：≥ 6.6（推荐 6.12 LTS）
- 架构：x86_64 / arm64 / riscv64
- 工具链：GCC ≥ 9 / Clang ≥ 14 / bpftool
- eBPF：libbpf ≥ 1.0

## 快速开始

```bash
# 编译 AI 核心模块
cd kbuild
make ai_core

# 编译 eBPF 程序
cd ../ebpf
make

# 运行测试
cd ../tests
python3 test_ai_core.py
```
