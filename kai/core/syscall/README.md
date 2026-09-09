# sys_infer() — 实际内核集成

本目录把 `sys_infer()`（系统调用 #548）从"实现代码"变成"真编译进内核"，
并提供开箱即用的用户态库、真实 AI 引擎适配与多场景测试。

## 文件

| 路径 | 说明 |
|---|---|
| `kai_syscall_core.c` | **编入 vmlinux 的 syscall 本体**（鉴权/限流/拷贝/引擎注册表/元数据回写） |
| `integrate_sys_infer.sh` | 一键打补丁：注册 syscall 表 + Kconfig/Makefile 接线 |
| `engine_kai_demo.c` | 演示引擎 .ko（本地规则，端到端验证） |
| `engines/engine_ai_core.c` | **真实引擎适配**：syscall548 → ai_core（缓存/量化/Layer2 DeepSeek+Kimi） |
| `libkai/` | **libkai 用户态库**：`kai_infer()` / `kai_infer_floats()` / `kai_syscall()` + demo |
| `tests/testsuite.c` | 多场景回归：并发 32×200 / 越界 / 超大输入 / NULL 容错 |
| `test_sys_infer.c` | 最小冒烟 |
| `Makefile` | `make test` / `make libkai` / `make engines` |
| `README.md` | 本文档 |

## 设计：syscall 常驻，后端热插拔

```
进程 → syscall 548 ──→ sys_kai_infer (编入 vmlinux)
                            │  kai_engine_register / unregister
                            ▼
                    推理引擎注册表（内核全局）
                      ├─ ai_core.ko 注册真实引擎（未来）
                      ├─ engine_kai_demo.ko（演示：本地规则）
                      └─ 无引擎 → -ENODEV
```

好处：系统调用永远在；AI 后端像插件一样 insmod 即可换。

## 集成步骤（真机，x86_64）

```bash
# 0. 前提：已解压 linux-6.12 源码树
./integrate_sys_infer.sh --kernel-src /usr/src/linux-6.12

# 1. 配置并编译内核
cd /usr/src/linux-6.12
make olddefconfig        # CONFIG_KAI_SYSCALL 默认 y
make -j$(nproc)          # 数分钟~数十分钟

# 2. 确认 syscall 进内核
grep kai_infer /proc/kallsyms      # sys_kai_infer

# 3. 编译并加载演示推理引擎
make -C /usr/src/linux-6.12 M=/usr/src/linux-6.12/kernel/kai modules
insmod kernel/kai/engine_kai_demo.ko

# 4. 用户态冒烟
gcc kai/core/syscall/test_sys_infer.c -o /tmp/tsi
/tmp/tsi     # 期望: OK 写入 16 字节 / score ≈ 0.45
```

## arm64 / 其它架构

asm-generic 宏表需要手动在 `include/uapi/asm-generic/unistd.h` 的
`#ifndef __NR_syscalls` 前插入：

```c
#define __NR_kai_infer 548
__SYSCALL(__NR_kai_infer, sys_kai_infer)
```

然后同样把 `kernel/kai/kai_syscall_core.c` + Kconfig/Makefile 接线编译。

## 让 ai_core 成为正式引擎

`engines/engine_ai_core.c` 已把 syscall → `ai_infer_sync` 接好：

```bash
insmod ai_core.ko          # 先有 Layer1/Layer2
insmod engine_ai_core.ko   # 注册为 syscall548 引擎
# 之后 sys_infer() 即走 缓存→量化→DeepSeek/Kimi 全链路
```

`engine_kai_demo.c` 是脱离 ai_core 也能单测的最小引擎模板。

## 用户态调用（libkai）

```bash
make libkai    # 生成 build/libkai.a + demo_sys_infer

# 你的程序：
#include "kai_syscall.h"
long n = kai_infer_floats("deepseek-chat", feat, 8, out, 2, &id, &lat);
```

## 多场景测试

```bash
make test
sudo ./build/testsuite     # 回归：并发/越界/超大/NULL
```

## 安全提示

- 默认只允许 `CAP_SYS_ADMIN` / `CAP_SYS_NICE` 调用
- 每 10 秒最多 200 次 + 并发上限 1024，超限返回 `-EAGAIN`
- 输入输出上限各 256KB，全部经 access_ok + copy_*_user
