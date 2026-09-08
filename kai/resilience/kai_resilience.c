// SPDX-License-Identifier: GPL-2.0
/*
 * kai_resilience.c — 可靠性保障实现
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/vmstat.h>

#include "kai_resilience.h"

#define DRV_NAME "kai_resilience"
#define DRV_VER  "1.0.0"

/* =========================================================================
 * 熔断器
 * ========================================================================= */

int circuit_breaker_init(struct circuit_breaker *cb,
                          __u32 failure_threshold,
                          __u32 timeout_ms)
{
    if (!cb)
        return -EINVAL;

    memset(cb, 0, sizeof(*cb));
    cb->failure_threshold = failure_threshold ?: 5;
    cb->success_threshold = 3;
    cb->timeout_ms = timeout_ms ?: 30000;
    cb->half_open_requests = 1;
    cb->state = CIRCUIT_CLOSED;
    atomic_set(&cb->failure_count, 0);
    atomic_set(&cb->success_count, 0);
    atomic_set(&cb->request_count, 0);
    atomic64_set(&cb->last_failure_ns, 0);
    atomic64_set(&cb->state_changed_ns, ktime_get_ns());
    spin_lock_init(&cb->lock);

    return 0;
}
EXPORT_SYMBOL_GPL(circuit_breaker_init);

int circuit_breaker_allow(struct circuit_breaker *cb)
{
    unsigned long flags;
    u64 now_ns;
    int allowed = 0;

    if (!cb)
        return -EINVAL;

    spin_lock_irqsave(&cb->lock, flags);
    now_ns = ktime_get_ns();

    switch (cb->state) {
    case CIRCUIT_CLOSED:
        allowed = 1;
        atomic_inc(&cb->request_count);
        break;

    case CIRCUIT_OPEN: {
        u64 elapsed_ms = (now_ns - atomic64_read(&cb->state_changed_ns)) / 1000000ULL;
        if (elapsed_ms > cb->timeout_ms) {
            /* 超时，进入半开状态 */
            cb->state = CIRCUIT_HALF_OPEN;
            atomic_set(&cb->success_count, 0);
            atomic_set(&cb->request_count, 0);
            atomic64_set(&cb->state_changed_ns, now_ns);
            allowed = 1;
        } else {
            allowed = 0; /* 熔断中，拒绝 */
        }
        break;
    }

    case CIRCUIT_HALF_OPEN:
        if (atomic_read(&cb->request_count) < cb->half_open_requests) {
            allowed = 1;
            atomic_inc(&cb->request_count);
        }
        break;
    }

    spin_unlock_irqrestore(&cb->lock, flags);
    return allowed;
}
EXPORT_SYMBOL_GPL(circuit_breaker_allow);

void circuit_breaker_success(struct circuit_breaker *cb)
{
    unsigned long flags;

    if (!cb)
        return;

    spin_lock_irqsave(&cb->lock, flags);

    switch (cb->state) {
    case CIRCUIT_CLOSED:
        atomic_set(&cb->failure_count, 0);
        break;

    case CIRCUIT_HALF_OPEN:
        if (atomic_inc_return(&cb->success_count) >= cb->success_threshold) {
            /* 恢复 */
            cb->state = CIRCUIT_CLOSED;
            atomic_set(&cb->failure_count, 0);
            atomic64_set(&cb->state_changed_ns, ktime_get_ns());
            pr_info("%s: circuit recovered\n", DRV_NAME);
        }
        break;

    default:
        break;
    }

    spin_unlock_irqrestore(&cb->lock, flags);
}
EXPORT_SYMBOL_GPL(circuit_breaker_success);

void circuit_breaker_failure(struct circuit_breaker *cb)
{
    unsigned long flags;

    if (!cb)
        return;

    spin_lock_irqsave(&cb->lock, flags);

    switch (cb->state) {
    case CIRCUIT_CLOSED:
        if (atomic_inc_return(&cb->failure_count) >= cb->failure_threshold) {
            /* 触发熔断 */
            cb->state = CIRCUIT_OPEN;
            atomic64_set(&cb->state_changed_ns, ktime_get_ns());
            pr_warn("%s: circuit opened after %d failures\n",
                    DRV_NAME, cb->failure_threshold);
        }
        break;

    case CIRCUIT_HALF_OPEN:
        /* 半开状态失败，重新熔断 */
        cb->state = CIRCUIT_OPEN;
        atomic64_set(&cb->state_changed_ns, ktime_get_ns());
        pr_warn("%s: circuit re-opened\n", DRV_NAME);
        break;

    default:
        break;
    }

    atomic64_set(&cb->last_failure_ns, ktime_get_ns());
    spin_unlock_irqrestore(&cb->lock, flags);
}
EXPORT_SYMBOL_GPL(circuit_breaker_failure);

void circuit_breaker_reset(struct circuit_breaker *cb)
{
    unsigned long flags;

    if (!cb)
        return;

    spin_lock_irqsave(&cb->lock, flags);
    cb->state = CIRCUIT_CLOSED;
    atomic_set(&cb->failure_count, 0);
    atomic_set(&cb->success_count, 0);
    atomic_set(&cb->request_count, 0);
    spin_unlock_irqrestore(&cb->lock, flags);
}
EXPORT_SYMBOL_GPL(circuit_breaker_reset);

enum circuit_state circuit_breaker_get_state(struct circuit_breaker *cb)
{
    return cb ? cb->state : CIRCUIT_OPEN;
}
EXPORT_SYMBOL_GPL(circuit_breaker_get_state);

/* =========================================================================
 * 超时控制
 * ========================================================================= */

int timeout_start(struct timeout_ctx *ctx, __u32 timeout_ms)
{
    if (!ctx)
        return -EINVAL;

    ctx->start_ns = ktime_get_ns();
    ctx->timeout_ms = timeout_ms;
    atomic_set(&ctx->expired, 0);

    return 0;
}
EXPORT_SYMBOL_GPL(timeout_start);

bool timeout_expired(struct timeout_ctx *ctx)
{
    if (!ctx)
        return true;

    if (atomic_read(&ctx->expired))
        return true;

    u64 elapsed = (ktime_get_ns() - ctx->start_ns) / 1000000ULL;
    if (elapsed >= ctx->timeout_ms) {
        atomic_set(&ctx->expired, 1);
        return true;
    }

    return false;
}
EXPORT_SYMBOL_GPL(timeout_expired);

__u64 timeout_remaining_ms(struct timeout_ctx *ctx)
{
    if (!ctx)
        return 0;

    if (atomic_read(&ctx->expired))
        return 0;

    u64 elapsed = (ktime_get_ns() - ctx->start_ns) / 1000000ULL;
    if (elapsed >= ctx->timeout_ms)
        return 0;

    return ctx->timeout_ms - elapsed;
}
EXPORT_SYMBOL_GPL(timeout_remaining_ms);

void timeout_cancel(struct timeout_ctx *ctx)
{
    if (ctx)
        atomic_set(&ctx->expired, 1);
}
EXPORT_SYMBOL_GPL(timeout_cancel);

/* =========================================================================
 * 资源限制
 * ========================================================================= */

int resource_check(const struct resource_limits *limits,
                   __u32 current_concurrent,
                   __u32 current_memory_mb,
                   __u32 current_cpu_pct)
{
    if (!limits)
        return 0;

    if (limits->max_concurrent_inferences &&
        current_concurrent > limits->max_concurrent_inferences)
        return -EAGAIN;

    if (limits->max_memory_mb &&
        current_memory_mb > limits->max_memory_mb)
        return -ENOMEM;

    if (limits->max_cpu_percent &&
        current_cpu_pct > limits->max_cpu_percent)
        return -EBUSY;

    return 0;
}
EXPORT_SYMBOL_GPL(resource_check);

/* =========================================================================
 * 内存压力保护
 * ========================================================================= */

static enum mem_pressure get_mem_pressure_level(void)
{
    struct sysinfo si;
    si_meminfo(&si);

    unsigned long total = si.totalram;
    unsigned long available = si.freeram + si.bufferram;
    int usage_pct = 0;

    if (total > 0)
        usage_pct = (int)((total - available) * 100 / total);

    if (usage_pct >= 95)
        return MEM_PRESSURE_CRITICAL;
    if (usage_pct >= 85)
        return MEM_PRESSURE_HIGH;
    if (usage_pct >= 70)
        return MEM_PRESSURE_MEDIUM;
    if (usage_pct >= 50)
        return MEM_PRESSURE_LOW;

    return MEM_PRESSURE_NONE;
}

int mem_protect_init(struct mem_protect *mp,
                     __u32 high_pct, __u32 critical_pct)
{
    if (!mp)
        return -EINVAL;

    memset(mp, 0, sizeof(*mp));
    mp->high_threshold_pct = high_pct ?: 85;
    mp->critical_threshold_pct = critical_pct ?: 95;
    atomic_set(&mp->current_pressure, MEM_PRESSURE_NONE);

    return 0;
}
EXPORT_SYMBOL_GPL(mem_protect_init);

int mem_protect_check(struct mem_protect *mp)
{
    enum mem_pressure level;
    int old_level;

    if (!mp)
        return -EINVAL;

    level = get_mem_pressure_level();
    old_level = atomic_read(&mp->current_pressure);
    atomic_set(&mp->current_pressure, level);

    /* 压力变化时回调 */
    if (level != old_level && mp->on_pressure)
        mp->on_pressure(level, mp->priv);

    /* 高压力时拒绝新请求 */
    if (level >= MEM_PRESSURE_HIGH)
        return -ENOMEM;

    return 0;
}
EXPORT_SYMBOL_GPL(mem_protect_check);

void mem_protect_release(struct mem_protect *mp, enum mem_pressure level)
{
    if (!mp)
        return;

    if (mp->on_release)
        mp->on_release(level, mp->priv);
}
EXPORT_SYMBOL_GPL(mem_protect_release);

/* =========================================================================
 * 降级链
 * ========================================================================= */

int fallback_chain_init(struct fallback_chain *fc,
                         enum fallback_level *levels, int count)
{
    if (!fc || !levels || count <= 0 || count > 4)
        return -EINVAL;

    memset(fc, 0, sizeof(*fc));
    memcpy(fc->levels, levels, count * sizeof(*levels));
    fc->num_levels = count;
    fc->current_level = 0;
    atomic_set(&fc->attempts, 0);

    return 0;
}
EXPORT_SYMBOL_GPL(fallback_chain_init);

int fallback_next(struct fallback_chain *fc)
{
    if (!fc || fc->current_level >= fc->num_levels)
        return -ENOENT;

    return fc->levels[fc->current_level++];
}
EXPORT_SYMBOL_GPL(fallback_next);

int fallback_get(struct fallback_chain *fc)
{
    if (!fc || fc->current_level >= fc->num_levels)
        return FALLBACK_NONE;

    return fc->levels[fc->current_level];
}
EXPORT_SYMBOL_GPL(fallback_get);

void fallback_reset(struct fallback_chain *fc)
{
    if (fc)
        fc->current_level = 0;
}
EXPORT_SYMBOL_GPL(fallback_reset);

/* =========================================================================
 * 健康检查
 * ========================================================================= */

int health_check(struct health_status *hs)
{
    if (!hs)
        return 0;

    hs->last_check_ns = ktime_get_ns();
    hs->check_count++;

    /* 基础检查 */
    hs->alive = true;
    hs->score = 100;

    /* 内存检查 */
    struct sysinfo si;
    si_meminfo(&si);
    int mem_pct = (int)((si.totalram - si.freeram) * 100 / si.totalram);
    if (mem_pct > 90) hs->score -= 20;
    else if (mem_pct > 70) hs->score -= 10;

    /* 熔断器状态检查 */
    /* 预留 */

    return hs->score;
}
EXPORT_SYMBOL_GPL(health_check);

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init kai_resilience_init(void)
{
    pr_info("========================================\n");
    pr_info("  KAI Resilience v%s\n", DRV_VER);
    pr_info("  Circuit breaker + Timeout + Memory protect\n");
    pr_info("========================================\n");
    return 0;
}

static void __exit kai_resilience_exit(void)
{
    pr_info("%s: unloaded\n", DRV_NAME);
}

module_init(kai_resilience_init);
module_exit(kai_resilience_exit);

MODULE_DESCRIPTION("KAI Resilience — Circuit breaker & protection");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
