# AI Linux 系统架构

## 1. 内核 AI 核心子系统（ai_core）

### 1.1 定位
在内核空间提供统一的 AI 推理接口，向上为调度器/IO/安全等子系统提供推理能力，向下对接用户态推理服务或内核模块内嵌的小模型。

### 1.2 核心数据结构

```c
struct ai_model {
    char             name[64];
    int              version;
    void            *weights;         // 指向模型权重（mmap 区域）
    size_t           weight_size;
    struct list_head list;            // 全局模型链表
    refcount_t       refcnt;
    rwlock_t         lock;
};

struct ai_inference_request {
    void            *input;           // 推理输入
    size_t           input_size;
    void            *output;          // 推理输出
    size_t           output_size;
    ai_model_t      *model;           // 使用的模型
    void            *priv;           // 调用者私有数据
    void            (*callback)(struct ai_inference_result *, void *);
};

struct ai_inference_result {
    int              err;
    void            *output;
    size_t           output_size;
    u64              latency_ns;
    void            *priv;
};
```

### 1.3 核心 API

```c
// 模型管理
ai_model_t *ai_model_register(const char *name, int version, void *weights, size_t size);
void ai_model_put(ai_model_t *model);
ai_model_t *ai_model_get(const char *name);

// 同步推理（阻塞，适合短路径如调度决策）
int ai_infer_sync(ai_model_t *model, void *input, size_t isize, void *output, size_t osize);

// 异步推理（通过 workqueue，不阻塞调度）
int ai_infer_async(struct ai_inference_request *req);

// 推理统计
void ai_stats_read(struct ai_stats *out);
```

## 2. 系统调用：sys_infer()

### 2.1 ABI 设计

```
sys_infer(
    unsigned long arg0,  // 模型句柄或名称
    unsigned long arg1,  // 输入缓冲区指针
    unsigned long arg2,  // 输入长度
    unsigned long arg3,  // 输出缓冲区指针
    unsigned long arg4   // 输出缓冲区长度
) -> long
```

返回值：
- ≥ 0：输出字节数
- -EFAULT：内存访问错误
- -ENOMEM：内存不足
- -EINVAL：参数无效
- -EAGAIN：推理服务未就绪（降级路径）

### 2.2 实现路径

```
用户态 libc
  → syscall(__NR_infer, ...)
  → do_sys_infer() [kernel/sys.c 或新建 ai_syscalls.c]
    → 权限检查（capability）
    → 内存拷贝（from_user / to_user）
    → ai_infer_sync() 或转发到推理服务
    → 返回结果
```

## 3. AI 调度器（sched_ext 插件）

### 3.1 原理
Linux 6.12 引入 `sched_ext`，允许用 BPF 程序完全替换 CFS 调度器。AI 调度器在 BPF 层做决策，内核提供调度原语。

### 3.2 决策输入特征（每任务）

| 特征 | 来源 | 说明 |
|---|---|---|
| `sum_exec_runtime` | `task_struct` | CPU 执行时间 |
| `nvcsw` | `task_struct` | 自愿切换 |
| `nivcsw` | `task_struct` | 非自愿切换 |
| `cpu_util` | `sched_avg` | CPU 利用率估算 |
| `avg_cache_util` | 扩展 | 缓存命中率（perf） |
| `io_wait` | `utime/stime` | IO 等待时间 |
| `vruntime` | `sched_entity` | 虚拟运行时间 |
| `prio` | `task_struct` | 优先级 |

### 3.3 调度策略输出

```c
enum ai_sched_decision {
    AI_PROMOTE,    // 升权（给更多时间片）
    AI_DEMOTE,     // 降权（减少时间片）
    AI_MIGRATE,    // 迁移到其他 CPU
    AI_BATCH,      // 标记为批处理
    AI_IDLE,       // 插入 idle
    AI_KEEP,       // 保持现状
};
```

### 3.4 BPF 调度器框架

```c
// ebpf/sched/ai_sched.bpf.c
SEC("sched_ext")
void sched_ai_select_cpu(struct bpf_sched_context *ctx)
{
    // 1. 收集特征
    struct ai_task_features f = collect_features(ctx->task);

    // 2. 发送推理请求
    long decision = ai_model_infer(ctx->task, &f, sizeof(f));

    // 3. 应用决策
    switch (decision) {
    case AI_MIGRATE:
        ctx->best_cpu = ai_choose_cpu(ctx, &f);
        break;
    case AI_PROMOTE:
        ctx->weight *= 1.2;
        break;
    case AI_DEMOTE:
        ctx->weight *= 0.8;
        break;
    }
}
```

## 4. AI IO（XDP/eBPF 层）

### 4.1 流量分类 XDP

```c
// ebpf/io/ai_xdp.bpf.c
SEC("xdp")
int ai_xdp_classify(struct xdp_md *ctx)
{
    void *data     = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;
    struct ethhdr *eth = data;

    // 提取 packet 特征
    struct packet_features pf;
    pf.src_ip = ...;
    pf.dst_ip = ...;
    pf.src_port = ...;
    pf.packet_len = data_end - data;

    // 推理异常分（0~1）
    float score = ai_packet_score(&pf, sizeof(pf));

    if (score > 0.9)
        return XDP_DROP;
    else if (score > 0.7)
        return XDP_PASS;  // 降速处理
    else
        return XDP_TX;     // 正常转发
}
```

### 4.2 存储预取

在 `block/bio` 层插入 AI 预测：基于历史 IO 模式预测下一个要预取的块，送到 `readahead` 逻辑。

## 5. AI 内存管理

### 5.1 kswapd 增强

在 `mm/vmscan.c` 的 `kswapd_balance()` 中加入 AI 预测：

```c
// ai_swap_predict() — 预测哪些页面即将被访问
// 输入：最近 30 个进程的 recent_refs
// 输出：有序页面列表（最可能被访问的先换入）
int ai_predict_swap(struct mem_cgroup *memcg, struct page **predictions, int max)
```

## 6. 安全检测（BPF LSM）

```c
// ebpf/security/ai_lsm.bpf.c
SEC("lsm/task_exec")
int AIHook_task_exec(struct linux_binprm *bprm)
{
    struct exec_features f = extract_exec_features(bprm);

    float score = ai_score_exec(&f, sizeof(f));
    if (score > THRESHOLD_ANOMALY)
        return -EPERM;  // 拒绝执行

    return 0;
}
```

## 7. AI 加速器驱动抽象（drivers/ai/）

```c
// drivers/ai/ai_hw.c — 统一抽象层
struct ai_device_ops {
    int  (*submit)(struct ai_device *, struct ai_task *);
    int  (*sync_run)(struct ai_device *, void *, size_t, void *, size_t);
    void (*wait)(struct ai_device *);
    int  (*get_result)(struct ai_device *, struct ai_task *, void *, size_t);
};

struct ai_device {
    char           name[64];
    enum ai_vendor vendor;       // NVIDIA / AMD / INTEL / HISI / CAMBRICON / SOFT
    struct ai_device_ops *ops;
    void           *priv;
};

enum ai_vendor {
    AI_VENDOR_SOFT       = 0,   // 软件模拟（ai_core.ko 内）
    AI_VENDOR_NVIDIA     = 1,
    AI_VENDOR_AMD        = 2,
    AI_VENDOR_INTEL_MLA  = 3,
    AI_VENDOR_HISI_Ascend = 4,
    AI_VENDOR_CAMBRICON  = 5,
    AI_VENDOR_CUSTOM     = 99,
};
```

## 8. 通信机制

| 路径 | 用途 | 延迟 |
|---|---|---|
| `sys_infer()` | 同步推理调用 | ~10-100μs（本地模型） |
| netlink (genetlink) | 配置/统计传递 | ~100μs |
| io_uring | 大批量推理请求 | ~50μs（零拷贝） |
| tracepoint + ring buffer | 推理输入数据采集 | ~1μs |
| /proc/ai/ | 运行时状态查看 | - |

## 9. 安全边界

- 所有推理输入/输出经过 `copy_from_user()` / `copy_to_user()`
-Capability 检查：`CAP_SYS_ADMIN` 才能注册/卸载模型
- BPF 验证器：eBPF 程序不能直接分配大块内存
- 模型权重 mmap 区域设置 `PG_reserved`，防止被换出
