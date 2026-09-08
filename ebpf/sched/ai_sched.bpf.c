// SPDX-License-Identifier: GPL-2.0
/*
 * ai_sched.bpf.c — AI 调度器 BPF 程序（sched_ext）
 *
 * 挂在 sched_ext 调度类上，替代或增强 CFS/EEVDF。
 *
 * 编译：
 *   clang -O2 -target bpf -g \
 *     -I/usr/include/bpf \
 *     -I/path/to/linux/tools/lib/bpf \
 *     ai_sched.bpf.c -o ai_sched.bpf.o
 *
 * 加载：
 *   bpftool sched replace \
 *     ./ai_sched.bpf.o \
 *     ai_sched \
 *     pin /sys/fs/bpf/sched_ext/ai_sched
 *
 * BPF 辅助函数（man bpf-helpers）：
 *   - bpf_probe_read_kernel()
 *   - bpf_map_update_elem()
 *   - bpf_per_cpu_ptr()
 *   - bpf_ktime_get_ns()
 */

#include <linux/bpf.h>
#include <linux/pid_namespace.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/bpf_sched.h>   /* Linux 6.12+ sched_ext API */
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* =========================================================================
 * BPF Maps — 存储推理结果和历史特征
 * ========================================================================= */

/* 任务级历史特征缓存
 * key: pid_tgid (由 bpf_get_current_pid_tgid() 获取)
 * value: struct ai_task_features
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key,   u64);                /* pid */
    __type(value, struct ai_task_features);
} ai_task_cache SEC(".maps");

/* 全局模型参数（演示用，真实模型来自用户态推理服务）
 * 通过 bpf() 系统调用或 netlink 从用户态推送
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 4);
    __type(key,   u32);
    __type(value, struct ai_model_params);
} ai_model_params SEC(".maps");

/* 推理结果输出环缓冲（用户态消费） */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 4096);
} ai_sched_events SEC(".maps");

/* =========================================================================
 * 数据结构
 * ========================================================================= */

/* 简化的模型参数（真实场景从用户态推送） */
struct ai_model_params {
    float weight[8];       /* 特征权重 */
    float bias;            /* 偏置 */
    float threshold_high;  /* 升权阈值 */
    float threshold_low;   /* 降权阈值 */
};

/* AI 调度决策 */
enum ai_sched_decision {
    AI_KEEP    = 0,
    AI_PROMOTE = 1,
    AI_DEMOTE  = 2,
    AI_MIGRATE = 3,
};

/* 推理结果事件 */
struct ai_sched_event {
    u32  pid;
    u32  cpu;
    u64  timestamp_ns;
    u32  decision;
    float score;
    u32  nvcsw;
    u32  nivcsw;
    u32  cpu_util;
};

/* =========================================================================
 * 辅助函数
 * ========================================================================= */

/* 从 task_struct 提取特征 */
static __always_inline void
collect_task_features(const struct task_struct *p,
                      struct ai_task_features *f)
{
    const struct sched_entity *se = &p->se;
    const struct sched_avg *sa = &p->se.avg;

    __builtin_memset(f, 0, sizeof(*f));

    /* 基本信息 */
    f->sum_exec_runtime = se->sum_exec_runtime;
    f->nvcsw            = p->nvcsw;
    f->nivcsw           = p->nivcsw;
    f->prio             = p->prio;

    /* CPU 利用率（0-1024） */
    f->cpu_util = sa->util_avg;

    /* NUMA 节点 */
    f->numa_node = p->numa_group ? p->numa_group->numa_node_id : 0;

    /* 进程名（最多16字节） */
    __builtin_memcpy(f->comm, p->comm, sizeof(p->comm) > 16 ? 16 : sizeof(p->comm));

    /* 内存信息需要额外辅助函数或 map */
    f->mem_usage_kb     = 0;   /* 需从 /proc/pid/status 读取 */
    f->io_wait_ns       = 0;   /* 需从 cgroup 或 schedstats 读取 */
    f->cache_miss_rate  = 0;
}

/* 最简评分函数：在 BPF 中做轻量特征加权
 * 真实场景：调用 ai_model_infer() 发送到用户态推理服务
 */
static __always_inline float
compute_score(const struct ai_task_features *f)
{
    /* 基于启发式评分（演示用）
     * 真实模型由用户态推理服务提供
     *
     * 指标：
     *   - 高 nivcsw（非自愿切换）= CPU 争用严重
     *   - 高 nvcsw / nivcsw 比 = IO 密集
     *   - 高 cpu_util = 计算密集
     *   - 低 prio (数值大) = 后台任务
     */

    float score = 0.5f;  /* 默认中性 */

    /* 非自愿切换率（越低越好，0-1） */
    float nivcsw_rate = f->nivcsw > 0 ?
        (float)f->nvcsw / (float)(f->nivcsw + 1) : 1.0f;
    score += (nivcsw_rate - 0.5f) * 0.3f;

    /* CPU 利用率归一化（0-1） */
    float cpu_ratio = (float)f->cpu_util / 1024.0f;
    score += (cpu_ratio - 0.5f) * 0.3f;

    /* 优先级偏移（低优先级任务可降权） */
    float prio_bias = (float)(f->prio - 120) / 40.0f; /* -3~+3 */
    score += prio_bias * 0.1f;

    /* 夹断到 [0,1] */
    if (score > 1.0f) score = 1.0f;
    if (score < 0.0f) score = 0.0f;

    return score;
}

/* 选择目标 CPU（用于迁移决策） */
static __always_inline int
choose_target_cpu(const struct ai_task_features *f,
                  int origin_cpu)
{
    /* 简单策略：选当前最空闲的 CPU
     * 真实场景：遍历 sched_domain 找最合适的
     */
    unsigned long min_load = ~0UL;
    int best_cpu = origin_cpu;

    /* 遍历所有 CPU（最多 64 个，BPF 循环限制） */
    for (int i = 0; i < 64; i++) {
        /* BPF 不允许直接访问其他 CPU 的负载
         * 真实场景：从 percpu 统计 map 中读取
         */
        (void)f;
        (void)min_load;
        (void)best_cpu;
    }

    return best_cpu;
}

/* =========================================================================
 * sched_ext 核心调度回调
 *
 * sched_ext 是 Linux 6.12 引入的 BPF 调度框架。
 * 编译目标内核需 CONFIG_SCHED_EXT=y
 * ========================================================================= */

#ifdef CONFIG_SCHED_EXT

SEC("sched_ext")
int ai_sched_select_cpu(struct bpf_sched_context *ctx)
{
    struct ai_task_features f = { 0 };
    const struct task_struct *p = ctx->task;
    u64 pid;
    float score;
    int decision;
    int target_cpu;

    if (!p)
        return -1;  /* 使用默认调度器 */

    pid = bpf_get_current_pid_tgid() >> 32;

    /* 1. 收集任务特征 */
    collect_task_features(p, &f);

    /* 2. 缓存特征（用于历史分析） */
    bpf_map_update_elem(&ai_task_cache, &pid, &f, BPF_ANY);

    /* 3. 计算 AI 评分 */
    score = compute_score(&f);

    /* 4. 映射评分到决策 */
    if (score > 0.7f)
        decision = AI_PROMOTE;
    else if (score < 0.3f)
        decision = AI_DEMOTE;
    else
        decision = AI_KEEP;

    /* 5. 特殊处理：IO 密集型任务（高自愿切换） */
    if (f.nvcsw > 1000 && f.nivcsw < 10) {
        /* IO 密集型：提升优先级，迁移到空闲 CPU */
        decision = AI_MIGRATE;
    }

    /* 6. 记录决策事件到 ring buffer */
    struct ai_sched_event *evt;
    evt = bpf_ringbuf_reserve(&ai_sched_events, sizeof(*evt), 0);
    if (evt) {
        evt->pid        = pid;
        evt->cpu        = bpf_get_smp_processor_id();
        evt->timestamp_ns = bpf_ktime_get_ns();
        evt->decision   = decision;
        evt->score      = score;
        evt->nvcsw      = f.nvcsw;
        evt->nivcsw     = f.nivcsw;
        evt->cpu_util   = f.cpu_util;
        bpf_ringbuf_submit(evt, 0);
    }

    /* 7. 应用调度决策 */
    target_cpu = bpf_get_smp_processor_id();

    switch (decision) {
    case AI_PROMOTE:
        /* 返回 -1 让调度器给任务更多时间片 */
        return target_cpu;

    case AI_DEMOTE:
        /* 提示调度器减少时间片 */
        return target_cpu;

    case AI_MIGRATE:
        target_cpu = choose_target_cpu(&f, target_cpu);
        return target_cpu;

    case AI_KEEP:
    default:
        /* 使用默认 CPU */
        return target_cpu;
    }
}

/* 任务入队回调 */
SEC("sched_ext")
int ai_sched_enqueue(struct bpf_sched_context *ctx)
{
    u64 pid = bpf_get_current_pid_tgid() >> 32;

    /* 可以在入队时做一些预处理 */
    struct ai_task_features *cached =
        bpf_map_lookup_elem(&ai_task_cache, &pid);

    if (cached) {
        /* 检查是否需要重新评估调度策略 */
        float score = compute_score(cached);
        if (score < 0.2f) {
            /* 非常不活跃的任务，可以降低其优先级 */
            /* 通过 ctx->scx 设置权重或 flags */
        }
    }

    return 0;
}

/* 任务出队回调 */
SEC("sched_ext")
int ai_sched_dequeue(struct bpf_sched_context *ctx)
{
    /* 任务被调度出去时调用 */
    return 0;
}

#endif /* CONFIG_SCHED_EXT */

/* =========================================================================
 * 通用 tracepoint 探针（不依赖 sched_ext）
 * ========================================================================= */

/* sched_switch tracepoint — 每次任务切换时触发
 * 这是一个调试/分析探针，不做调度决策
 */
SEC("tracepoint/sched/sched_switch")
int ai_trace_sched_switch(struct bpf_sched_switch_args *ctx)
{
    u64 pid = ctx->prev_pid;
    u64 now = bpf_ktime_get_ns();
    u64 delta_ns;
    struct ai_task_features *prev;
    struct ai_task_features f = { 0 };

    prev = bpf_map_lookup_elem(&ai_task_cache, &pid);
    if (prev) {
        delta_ns = now - prev->sum_exec_runtime;

        /* 更新历史 */
        f = *prev;
        f.sum_exec_runtime = now;  /* 复用字段存储上次切换时间 */
        bpf_map_update_elem(&ai_task_cache, &pid, &f, BPF_ANY);
    }

    return 0;
}

/* =========================================================================
 * License
 * ========================================================================= */
char _license[] SEC("license") = "GPL";
