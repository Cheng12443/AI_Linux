// SPDX-License-Identifier: GPL-2.0
/*
 * kai_metrics.c — 监控指标实现
 *
 * 提供：
 *   - Prometheus 指标导出
 *   - 结构化日志
 *   - 审计日志
 *   - 告警引擎
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/printk.h>

#include "kai_metrics.h"

#define DRV_NAME "kai_metrics"
#define DRV_VER  "1.0.0"

/* 全局注册表 */
static struct metric_registry g_registry;
static DEFINE_SPINLOCK(g_lock);

/* 日志级别 */
static enum log_level g_log_level = LOG_INFO;

/* 审计缓冲区 */
#define AUDIT_BUF_SIZE 256
static struct audit_entry g_audit_buf[AUDIT_BUF_SIZE];
static int g_audit_idx = 0;
static DEFINE_SPINLOCK(g_audit_lock);

/* 告警规则 */
static LIST_HEAD(g_alert_rules);
static DEFINE_MUTEX(g_alert_lock);

/* =========================================================================
 * 指标注册
 * ========================================================================= */

int metric_register(struct metric_registry *reg,
                    const char *name,
                    const char *help,
                    enum metric_type type,
                    const char *labels)
{
    struct metric *m;

    if (!reg || !name)
        return -EINVAL;

    m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m)
        return -ENOMEM;

    strscpy(m->name, name, sizeof(m->name));
    if (help)
        strscpy(m->help, help, sizeof(m->help));
    m->type = type;
    m->value = 0;
    m->created_ns = ktime_get_ns();
    m->updated_ns = m->created_ns;
    if (labels)
        strscpy(m->labels, labels, sizeof(m->labels));

    spin_lock(&g_lock);
    list_add(&m->list, &reg->metrics);
    reg->count++;
    spin_unlock(&g_lock);

    return 0;
}
EXPORT_SYMBOL_GPL(metric_register);

int metric_set(struct metric_registry *reg,
               const char *name, __u64 value)
{
    struct metric *m;
    unsigned long flags;

    spin_lock_irqsave(&g_lock, flags);
    list_for_each_entry(m, &reg->metrics, list) {
        if (strcmp(m->name, name) == 0) {
            m->value = value;
            m->updated_ns = ktime_get_ns();
            spin_unlock_irqrestore(&g_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_lock, flags);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(metric_set);

int metric_inc(struct metric_registry *reg, const char *name)
{
    struct metric *m;
    unsigned long flags;

    spin_lock_irqsave(&g_lock, flags);
    list_for_each_entry(m, &reg->metrics, list) {
        if (strcmp(m->name, name) == 0) {
            m->value++;
            m->updated_ns = ktime_get_ns();
            spin_unlock_irqrestore(&g_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_lock, flags);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(metric_inc);

int metric_dec(struct metric_registry *reg, const char *name)
{
    struct metric *m;
    unsigned long flags;

    spin_lock_irqsave(&g_lock, flags);
    list_for_each_entry(m, &reg->metrics, list) {
        if (strcmp(m->name, name) == 0) {
            if (m->value > 0)
                m->value--;
            m->updated_ns = ktime_get_ns();
            spin_unlock_irqrestore(&g_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_lock, flags);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(metric_dec);

int metric_add(struct metric_registry *reg, const char *name, __u64 delta)
{
    struct metric *m;
    unsigned long flags;

    spin_lock_irqsave(&g_lock, flags);
    list_for_each_entry(m, &reg->metrics, list) {
        if (strcmp(m->name, name) == 0) {
            m->value += delta;
            m->updated_ns = ktime_get_ns();
            spin_unlock_irqrestore(&g_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_lock, flags);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(metric_add);

int metric_get(struct metric_registry *reg, const char *name, __u64 *value)
{
    struct metric *m;
    unsigned long flags;

    spin_lock_irqsave(&g_lock, flags);
    list_for_each_entry(m, &reg->metrics, list) {
        if (strcmp(m->name, name) == 0) {
            if (value)
                *value = m->value;
            spin_unlock_irqrestore(&g_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_lock, flags);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(metric_get);

void metric_unregister(struct metric_registry *reg, const char *name)
{
    struct metric *m, *tmp;
    unsigned long flags;

    spin_lock_irqsave(&g_lock, flags);
    list_for_each_entry_safe(m, tmp, &reg->metrics, list) {
        if (strcmp(m->name, name) == 0) {
            list_del(&m->list);
            reg->count--;
            kfree(m);
            spin_unlock_irqrestore(&g_lock, flags);
            return;
        }
    }
    spin_unlock_irqrestore(&g_lock, flags);
}
EXPORT_SYMBOL_GPL(metric_unregister);

/* =========================================================================
 * Prometheus 格式导出
 * ========================================================================= */

int metric_format_prometheus(struct metric_registry *reg,
                              char *buf, size_t size)
{
    struct metric *m;
    unsigned long flags;
    size_t used = 0;

    spin_lock_irqsave(&g_lock, flags);

    list_for_each_entry(m, &reg->metrics, list) {
        /* # HELP */
        if (m->help[0]) {
            used += snprintf(buf + used, size - used,
                           "# HELP %s %s\n", m->name, m->help);
        }

        /* # TYPE */
        const char *type_str = "gauge";
        switch (m->type) {
        case METRIC_COUNTER:   type_str = "counter";   break;
        case METRIC_HISTOGRAM:type_str = "histogram";  break;
        case METRIC_SUMMARY:   type_str = "summary";   break;
        }
        used += snprintf(buf + used, size - used,
                       "# TYPE %s %s\n", m->name, type_str);

        /* 指标值 */
        if (m->labels[0]) {
            used += snprintf(buf + used, size - used,
                           "%s{%s} %llu\n",
                           m->name, m->labels, m->value);
        } else {
            used += snprintf(buf + used, size - used,
                           "%s %llu\n", m->name, m->value);
        }

        used += snprintf(buf + used, size - used, "\n");
    }

    spin_unlock_irqrestore(&g_lock, flags);
    return used;
}
EXPORT_SYMBOL_GPL(metric_format_prometheus);

/* =========================================================================
 * 结构化日志
 * ========================================================================= */

static void format_log(struct log_entry *entry, char *buf, size_t size)
{
    struct tm tm;
    time_t t = entry->timestamp_ns / 1000000000ULL;
    time_to_tm(t, 0, &tm);

    const char *level_str[] = {"DEBUG", "INFO ", "WARN ", "ERROR", "AUDIT"};

    snprintf(buf, size,
            "{\"timestamp\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\","
            "\"level\":\"%s\","
            "\"component\":\"%s\","
            "\"message\":\"%s\","
            "\"pid\":%llu,"
            "\"uid\":%u}\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            (int)((entry->timestamp_ns % 1000000000ULL) / 1000000ULL),
            level_str[entry->level],
            entry->component,
            entry->message,
            entry->pid,
            entry->uid);
}

int kai_log(enum log_level level,
            const char *component,
            const char *fmt, ...)
{
    va_list args;
    struct log_entry entry;

    if (level < g_log_level)
        return 0;

    entry.timestamp_ns = ktime_get_ns();
    entry.level = level;
    strscpy(entry.component, component ?: "kai", sizeof(entry.component));
    entry.pid = task_pid_nr(current);
    entry.uid = from_kuid(&init_user_ns, current_uid());

    va_start(args, fmt);
    vsnprintf(entry.message, sizeof(entry.message), fmt, args);
    va_end(args);

    /* 输出到内核日志 */
    switch (level) {
    case LOG_ERROR: pr_err("[%s] %s", component, entry.message); break;
    case LOG_WARN:  pr_warn("[%s] %s", component, entry.message); break;
    case LOG_AUDIT: pr_info("[AUDIT][%s] %s", component, entry.message); break;
    default:        pr_info("[%s] %s", component, entry.message); break;
    }

    return 0;
}
EXPORT_SYMBOL_GPL(kai_log);

void kai_set_log_level(enum log_level level)
{
    g_log_level = level;
}
EXPORT_SYMBOL_GPL(kai_set_log_level);

/* =========================================================================
 * 审计日志
 * ========================================================================= */

int kai_audit(enum audit_type type,
              const char *action,
              const char *target,
              const char *result,
              const char *details)
{
    struct audit_entry *entry;
    unsigned long flags;

    spin_lock_irqsave(&g_audit_lock, flags);

    entry = &g_audit_buf[g_audit_idx];
    entry->timestamp_ns = ktime_get_ns();
    entry->type = type;
    entry->uid = from_kuid(&init_user_ns, current_uid());
    entry->pid = task_pid_nr(current);
    strscpy(entry->comm, current->comm, sizeof(entry->comm));
    strscpy(entry->action, action ?: "unknown", sizeof(entry->action));
    strscpy(entry->target, target ?: "", sizeof(entry->target));
    strscpy(entry->result, result ?: "success", sizeof(entry->result));
    if (details)
        strscpy(entry->details, details, sizeof(entry->details));

    g_audit_idx = (g_audit_idx + 1) % AUDIT_BUF_SIZE;

    spin_unlock_irqrestore(&g_audit_lock, flags);

    /* 同时输出到日志 */
    kai_log(LOG_AUDIT, "audit",
            "%s: %s -> %s (%s)",
            action, target, result, details ?: "");

    return 0;
}
EXPORT_SYMBOL_GPL(kai_audit);

int kai_audit_dump(char *buf, size_t size)
{
    int i;
    size_t used = 0;

    used += snprintf(buf + used, size - used,
                    "Audit Log (%d entries):\n", AUDIT_BUF_SIZE);

    spin_lock(&g_audit_lock);

    for (i = 0; i < AUDIT_BUF_SIZE; i++) {
        struct audit_entry *e = &g_audit_buf[i];
        if (e->timestamp_ns == 0)
            continue;

        used += snprintf(buf + used, size - used,
            "[%llu] uid=%u pid=%llu comm=%s %s %s -> %s\n",
            e->timestamp_ns, e->uid, e->pid, e->comm,
            e->action, e->target, e->result);
    }

    spin_unlock(&g_audit_lock);
    return used;
}
EXPORT_SYMBOL_GPL(kai_audit_dump);

/* =========================================================================
 * 告警引擎
 * ========================================================================= */

int alert_rule_add(struct alert_rule *rule)
{
    if (!rule)
        return -EINVAL;

    mutex_lock(&g_alert_lock);
    list_add(&rule->list, &g_alert_rules);
    mutex_unlock(&g_alert_lock);

    return 0;
}
EXPORT_SYMBOL_GPL(alert_rule_add);

int alert_rule_remove(const char *name)
{
    struct alert_rule *r, *tmp;

    mutex_lock(&g_alert_lock);
    list_for_each_entry_safe(r, tmp, &g_alert_rules, list) {
        if (strcmp(r->name, name) == 0) {
            list_del(&r->list);
            kfree(r);
            mutex_unlock(&g_alert_lock);
            return 0;
        }
    }
    mutex_unlock(&g_alert_lock);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(alert_rule_remove);

int alert_evaluate(struct metric_registry *reg)
{
    struct alert_rule *r;
    int triggered = 0;
    u64 now = ktime_get_ns();

    mutex_lock(&g_alert_lock);

    list_for_each_entry(r, &g_alert_rules, list) {
        if (!r->enabled)
            continue;

        /* 简化：只检查 CPU 使用率 */
        __u64 cpu_val;
        if (metric_get(reg, "cpu_usage", &cpu_val) == 0) {
            if (cpu_val > 90) {
                /* 冷却检查 */
                u64 cooldown = r->cooldown_ms * 1000000ULL;
                if (now - r->last_trigger_ns > cooldown) {
                    r->current_count++;
                    if (r->current_count >= r->repeat_count) {
                        triggered++;
                        r->last_trigger_ns = now;
                        kai_log(LOG_WARN, "alert",
                                "rule '%s' triggered (value=%llu)",
                                r->name, cpu_val);
                    }
                }
            } else {
                r->current_count = 0;
            }
        }
    }

    mutex_unlock(&g_alert_lock);
    return triggered;
}
EXPORT_SYMBOL_GPL(alert_evaluate);

/* =========================================================================
 * proc 接口
 * ========================================================================= */

static int metrics_show(struct seq_file *m, void *v)
{
    char buf[8192];
    int len;

    len = metric_format_prometheus(&g_registry, buf, sizeof(buf));
    seq_write(m, buf, len);
    return 0;
}

static int metrics_open(struct inode *inode, struct file *file)
{
    return single_open(file, metrics_show, NULL);
}

static const struct proc_ops metrics_proc_fops = {
    .proc_open    = metrics_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init kai_metrics_init(void)
{
    pr_info("========================================\n");
    pr_info("  KAI Metrics v%s\n", DRV_VER);
    pr_info("  Prometheus + Audit + Alert\n");
    pr_info("========================================\n");

    /* 初始化注册表 */
    INIT_LIST_HEAD(&g_registry.metrics);
    spin_lock_init(&g_registry.lock);
    g_registry.count = 0;

    /* 注册默认指标 */
    metric_register(&g_registry, "kai_inferences_total",
                     "Total AI inferences", METRIC_COUNTER, NULL);
    metric_register(&g_registry, "kai_inference_errors_total",
                     "Total inference errors", METRIC_COUNTER, NULL);
    metric_register(&g_registry, "kai_inference_latency_ns",
                     "Inference latency", METRIC_HISTOGRAM, NULL);
    metric_register(&g_registry, "kai_cache_hits_total",
                     "Cache hits", METRIC_COUNTER, NULL);
    metric_register(&g_registry, "kai_cache_misses_total",
                     "Cache misses", METRIC_COUNTER, NULL);
    metric_register(&g_registry, "kai_api_calls_total",
                     "API calls (DeepSeek/Kimi)", METRIC_COUNTER, NULL);
    metric_register(&g_registry, "kai_local_calls_total",
                     "Local inference calls", METRIC_COUNTER, NULL);
    metric_register(&g_registry, "kai_fallback_calls_total",
                     "Fallback rule calls", METRIC_COUNTER, NULL);
    metric_register(&g_registry, "cpu_usage",
                     "CPU usage percent", METRIC_GAUGE, NULL);
    metric_register(&g_registry, "mem_usage",
                     "Memory usage percent", METRIC_GAUGE, NULL);

    /* proc 接口 */
    proc_create("kai_metrics", 0444, NULL, &metrics_proc_fops);

    pr_info("%s: ready\n", DRV_NAME);
    return 0;
}

static void __exit kai_metrics_exit(void)
{
    struct metric *m, *tmp;

    remove_proc_entry("kai_metrics", NULL);

    /* 清理所有指标 */
    spin_lock(&g_lock);
    list_for_each_entry_safe(m, tmp, &g_registry.metrics, list) {
        list_del(&m->list);
        kfree(m);
    }
    spin_unlock(&g_lock);

    pr_info("%s: unloaded\n", DRV_NAME);
}

module_init(kai_metrics_init);
module_exit(kai_metrics_exit);

MODULE_DESCRIPTION("KAI Metrics — Prometheus + Audit + Alert");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
