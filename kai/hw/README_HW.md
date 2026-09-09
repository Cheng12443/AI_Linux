# KAI Linux — AI 硬件接入指南

本目录提供 AI 加速器的统一抽象层，屏蔽不同厂商 SDK 的差异。

## 支持的硬件

| 硬件 | SDK | 文件 | 备注 |
|---|---|---|---|
| NVIDIA GPU | CUDA/PyTorch | `backends.py` | nvidia-smi 自动探测 |
| AMD GPU | ROCm | `backends.py` | rocm-smi / torch.hip |
| Intel NPU/Gaudi | OpenVINO | `backends.py` | 自动探测 |
| 华为昇腾 | CANN/torch_npu | `backends.py` | npu-smi 自动探测 |
| 昆仑芯 XPU | Paddle Lite | `backends.py` | paddle 自动探测 |
| RISC-V NPU | 扩展指令 | `ai_accel.h` | 需自定义后端 |

## 快速使用

```bash
# 1. 安装对应 SDK（按需）
pip install torch          # NVIDIA (CUDA 版)
pip install torch --index-url https://download.pytorch.org/whl/rocm5.6
pip install openvino       # Intel
# 华为昇腾需安装 CANN 工具链

# 2. 探测硬件
python3 kai/hw/backends.py

# 3. 推理（自动选最优硬件）
python3 -c "
from backends import HardwareManager
hm = HardwareManager()
print(hm.summary())
"
```

## 输出示例

```
=== AI 硬件探测结果 ===
  ● [nvidia] NVIDIA A100-SXM4-40GB 40960MB
  ● [intel] CPU
```

## 内核级抽象（C）

`ai_accel.h` 提供内核态统一接口 `struct accel_ops`：

- `probe()` — 探测设备
- `alloc_mem()` / `copy_host_to_dev()` — 内存管理
- `load_model()` / `infer()` — 模型与推理
- `matmul()` / `conv2d()` — 张量运算

各厂商需实现这些接口（类似 GPU 驱动注册到 DRM/KFD 框架），
示例参见 `drivers/ai/`（骨架）。

## 统一推理链

```
用户请求
   │
   ▼
kai_infer（内核）
   │  优先
   ▼
ai_accel（硬件抽象层）
   ├── NVIDIA → CUDA 后端
   ├── AMD    → ROCm 后端
   ├── Intel  → OpenVINO 后端
   ├── 华为   → CANN 后端
   └── CPU    → 兜底
```
