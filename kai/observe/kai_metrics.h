/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kai_metrics.h — Prometheus 指标 + 结构化日志 + 审计
 *
 * 功能：
 *   - Prometheus 指标导出
 *   - 结构化 JSON 日志
 *   - 审计日志（安全事件）
 *   - 告警引擎（阈值告警）
 *   - Webhook 通知
 */

#ifndef _KAI_METRICS_H
#define _KAI_METRICS_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/list.h>

/* =========================================================================
 * Prometheus 指标
 * ========================================================================= */

/* 指标类型 */
enum metric_type {
    METRIC_COUNTER = 0,   /* 只增不减 */
    METRIC_GAUGE   = 1,   /* 可增可减 */
    METRIC_HISTOGRAM = 2, /* 直方图 */
    METRIC_SUMMARY = 3,   /* 摘要 */
};

struct metric {
    char        name[64];
    char        help[256];
    enum metric_type type;
    __u64       value;
    __u64       created_ns;
    __u64       updated_ns;

    /* 标签 */
    char        labels[512];  /* "key1=val1,key2=val2" */

    struct list_head list;
};

/* 指标注册表 */
struct metric_registry {
    struct list_head metrics;
    spinlock_t       lock;
    __u32            count;
};

int  metric_register(struct metric_registry *reg,
                     const char *name,
                     const char *help,
                     enum metric_type type,
                     const char *labels);
int  metric_set(struct metric_registry *reg,
                const char *name, __u64 value);
int  metric_inc(struct metric_registry *reg, const char *name);
int  metric_dec(struct metric_registry *reg, const char *name);
int  metric_add(struct metric_registry *reg, const char *name, __u64 delta);
int  metric_get(struct metric_registry *reg, const char *name, __u64 *value);
void metric_unregister(struct metric_registry *reg, const char *name);
int  metric_format_prometheus(struct metric_registry *reg,
                               char *buf, size_t size);

/* =========================================================================
 * 结构化日志
 * ========================================================================= */

enum log_level {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
    LOG_AUDIT = 4,
};

struct log_entry {
    __u64       timestamp_ns;
    enum log_level level;
    char        component[32];
    char        message[256];
    __u64       pid;
    __u32       uid;
    char        extra[256];  /* JSON 扩展字段 */
};

int kai_log(enum log_level level,
            const char *component,
            const char *fmt, ...);
int kai_log_json(char *buf, size_t size);

/* 日志级别控制 */
void kai_set_log_level(enum log_level level);

/* =========================================================================
 * 审计日志
 * ========================================================================= */

enum audit_type {
    AUDIT_INFERENCE    = 0,  /* 推理调用 */
    AUDIT_MODEL_LOAD   = 1,  /* 模型加载 */
    AUDIT_MODEL_UNLOAD = 2,  /* 模型卸载 */
    AUDIT_CONFIG_CHANGE = 3, /* 配置变更 */
    AUDIT_SECURITY     = 4,  /* 安全事件 */
    AUDIT_API_CALL     = 5,  /* API 调用 */
    AUDIT_PLUGIN       = 6,  /* 插件事件 */
};

struct audit_entry {
    __u64       timestamp_ns;
    enum audit_type type;
    __u32       uid;
    __u32       pid;
    char        comm[16];
    char        action[64];
    char        target[128];
    char        result[32];
    char        details[256];
};

int kai_audit(enum audit_type type,
              const char *action,
              const char *target,
              const char *result,
              const char *details);
int kai_audit_dump(char *buf, size_t size);

/* =========================================================================
 * 告警引擎
 * ========================================================================= */

enum alert_level {
    ALERT_INFO     = 0,
    ALERT_WARNING  = 1,
    ALERT_CRITICAL = 2,
};

struct alert_rule {
    char        name[64];
    char        expr[256];     /* 表达式，如 "cpu_usage > 90" */
    enum alert_level level;
    __u32       cooldown_ms;   /* 冷却时间 */
    __u32       repeat_count;  /* 连续触发次数 */
    bool        enabled;

    /* 内部状态 */
    __u32       current_count;
    __u64       last_trigger_ns;

    struct list_head list;
};

struct alert_event {
    __u64       timestamp_ns;
    enum alert_level level;
    char        rule_name[64];
    char        message[256];
    __u64       value;
    __u64       threshold;
};

int  alert_rule_add(struct alert_rule *rule);
int  alert_rule_remove(const char *name);
int  alert_rule_enable(const char *name, bool enabled);
int  alert_evaluate(struct metric_registry *reg);
int  alert_get_events(char *buf, size_t size);

/* =========================================================================
 * Webhook 通知
 * ========================================================================= */

int webhook_notify(const char *url,
                   enum alert_level level,
                   const char *title,
                   const char *message);

#endif /* _KAI_METRICS_H */
