/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kai_resilience.h — 可靠性保障系统
 *
 * 功能：
 *   - 熔断器（Circuit Breaker）
 *   - 超时控制（Timeout）
 *   - 内存压力保护（OOM Protection）
 *   - 资源限制（Resource Limits）
 *   - 降级链（Fallback Chain）
 *   - 健康检查（Health Check）
 */

#ifndef _KAI_RESILIENCE_H
#define _KAI_RESILIENCE_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>

/* =========================================================================
 * 熔断器（Circuit Breaker）
 * ========================================================================= */

/* 熔断器状态 */
enum circuit_state {
    CIRCUIT_CLOSED    = 0,  /* 正常 */
    CIRCUIT_OPEN      = 1,  /* 熔断（拒绝请求）*/
    CIRCUIT_HALF_OPEN = 2,  /* 半开（试探性恢复）*/
};

struct circuit_breaker {
    atomic_t           failure_count;    /* 连续失败次数 */
    atomic_t           success_count;    /* 半开状态成功次数 */
    atomic_t           request_count;    /* 请求总数 */
    enum circuit_state state;            /* 当前状态 */

    /* 配置 */
    __u32              failure_threshold;    /* 熔断阈值（默认 5）*/
    __u32              success_threshold;    /* 恢复阈值（默认 3）*/
    __u32              timeout_ms;           /* 熔断持续时间（默认 30000ms）*/
    __u32              half_open_requests;   /* 半开状态允许的最大请求数 */

    /* 时间戳 */
    atomic64_t         last_failure_ns;
    atomic64_t         state_changed_ns;

    spinlock_t         lock;
};

/* 熔断器 API */
int  circuit_breaker_init(struct circuit_breaker *cb,
                           __u32 failure_threshold,
                           __u32 timeout_ms);
int  circuit_breaker_allow(struct circuit_breaker *cb);
void circuit_breaker_success(struct circuit_breaker *cb);
void circuit_breaker_failure(struct circuit_breaker *cb);
void circuit_breaker_reset(struct circuit_breaker *cb);
enum circuit_state circuit_breaker_get_state(struct circuit_breaker *cb);

/* =========================================================================
 * 超时控制
 * ========================================================================= */

struct timeout_ctx {
    __u64 start_ns;
    __u32 timeout_ms;
    atomic_t expired;
};

int  timeout_start(struct timeout_ctx *ctx, __u32 timeout_ms);
bool timeout_expired(struct timeout_ctx *ctx);
__u64 timeout_remaining_ms(struct timeout_ctx *ctx);
void timeout_cancel(struct timeout_ctx *ctx);

/* =========================================================================
 * 资源限制
 * ========================================================================= */

struct resource_limits {
    __u32 max_concurrent_inferences;   /* 最大并发推理数 */
    __u32 max_memory_mb;               /* 最大内存（MB）*/
    __u32 max_cpu_percent;              /* 最大 CPU 百分比 */
    __u32 max_model_cache_mb;           /* 模型缓存上限 */
    __u32 max_batch_size;               /* 最大批处理大小 */
    __u32 inference_timeout_ms;         /* 推理超时 */
};

int resource_check(const struct resource_limits *limits,
                   __u32 current_concurrent,
                   __u32 current_memory_mb,
                   __u32 current_cpu_pct);

/* =========================================================================
 * 内存压力保护
 * ========================================================================= */

enum mem_pressure {
    MEM_PRESSURE_NONE   = 0,
    MEM_PRESSURE_LOW    = 1,
    MEM_PRESSURE_MEDIUM = 2,
    MEM_PRESSURE_HIGH   = 3,
    MEM_PRESSURE_CRITICAL = 4,
};

struct mem_protect {
    /* 回调 */
    int (*on_pressure)(enum mem_pressure level, void *data);
    void (*on_release)(enum mem_pressure level, void *data);
    void *priv;

    /* 阈值 */
    __u32 high_threshold_pct;      /* 高压力阈值 */
    __u32 critical_threshold_pct;  /* 严重压力阈值 */

    /* 状态 */
    atomic_t current_pressure;
};

int  mem_protect_init(struct mem_protect *mp,
                       __u32 high_pct, __u32 critical_pct);
int  mem_protect_check(struct mem_protect *mp);
void mem_protect_release(struct mem_protect *mp, enum mem_pressure level);

/* =========================================================================
 * 降级链
 * ========================================================================= */

/* 降级级别 */
enum fallback_level {
    FALLBACK_NONE     = 0,  /* 不降级 */
    FALLBACK_CACHE    = 1,  /* 用缓存结果 */
    FALLBACK_LOCAL    = 2,  /* 用本地模型 */
    FALLBACK_RULE    = 3,  /* 用规则引擎 */
    FALLBACK_DEFAULT = 4,  /* 用默认值 */
};

struct fallback_chain {
    enum fallback_level levels[4];
    int                 num_levels;
    int                 current_level;
    atomic_t            attempts;
};

int  fallback_chain_init(struct fallback_chain *fc,
                         enum fallback_level *levels, int count);
int  fallback_next(struct fallback_chain *fc);
int  fallback_get(struct fallback_chain *fc);
void fallback_reset(struct fallback_chain *fc);

/* =========================================================================
 * 健康检查
 * ========================================================================= */

struct health_status {
    bool alive;
    int  score;             /* 0-100 */
    __u64 last_check_ns;
    __u64 check_count;
    __u64 error_count;
};

int health_check(struct health_status *hs);
int health_register_check(int (*check_fn)(void *), void *data);

#endif /* _KAI_RESILIENCE_H */
