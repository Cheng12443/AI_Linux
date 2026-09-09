// SPDX-License-Identifier: GPL-2.0
/*
 * ai_plugin_mgr.c — AI Linux 插件管理器
 *
 * 职责：
 *   - 插件注册 / 注销
 *   - 插件查找与遍历
 *   - 推理分发（轮询 / 优先级 / 链式）
 *   - 事件通知
 *   - proc 接口
 *   - 热插拔支持
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/refcount.h>
#include <linux/rcupdate.h>

#include "ai_plugin.h"

#define DRV_NAME  "ai_plugin_mgr"
#define DRV_VER   "1.0.0"

/* =========================================================================
 * 全局数据结构
 * ========================================================================= */

/* 全局插件链表（所有插件）*/
static LIST_HEAD(all_plugins);
static DEFINE_MUTEX(plugins_lock);

/* 按类型组织的链表 */
static struct list_head plugins_by_type[8] = {
    [0] = LIST_HEAD_INIT(plugins_by_type[0]),
    [1] = LIST_HEAD_INIT(plugins_by_type[1]),
    [2] = LIST_HEAD_INIT(plugins_by_type[2]),
    [3] = LIST_HEAD_INIT(plugins_by_type[3]),
    [4] = LIST_HEAD_INIT(plugins_by_type[4]),
    [5] = LIST_HEAD_INIT(plugins_by_type[5]),
    [6] = LIST_HEAD_INIT(plugins_by_type[6]),
    [7] = LIST_HEAD_INIT(plugins_by_type[7]),
};

/* 事件订阅者 */
#define MAX_SUBSCRIBERS 64
static struct {
    __u32 event_type;
    ai_plugin_event_cb cb;
    void *data;
    int valid;
} subscribers[MAX_SUBSCRIBERS];
static DEFINE_SPINLOCK(subscriber_lock);

/* 全局统计 */
static __u64 g_total_inferences;
static __u64 g_total_errors;
static __u64 g_total_latency_ns;

/* proc */
static struct proc_dir_entry *ai_plugins_dir;

/* =========================================================================
 * 插件注册
 * ========================================================================= */

int ai_plugin_register(struct ai_plugin_ops *ops)
{
    struct ai_plugin *plugin;
    int type_idx;

    if (!ops || !ops->name || !ops->init)
        return -EINVAL;

    if (ops->api_version != AI_PLUGIN_VERSION) {
        pr_warn("%s: plugin '%s' API version mismatch "
                "(got 0x%x, want 0x%x)\n",
                DRV_NAME, ops->name, ops->api_version, AI_PLUGIN_VERSION);
        return -EPROTO;
    }

    plugin = kzalloc(sizeof(*plugin), GFP_KERNEL);
    if (!plugin)
        return -ENOMEM;

    plugin->ops   = ops;
    strscpy(plugin->name, ops->name, AI_PLUGIN_NAME_LEN - 1);
    plugin->type   = ops->type & PLUGIN_TYPE_MASK;
    plugin->flags  = PLUGIN_F_LOADED;
    refcount_set(&plugin->refcnt, 1);
    atomic_set(&plugin->enabled, 0);
    atomic_set(&plugin->busy, 0);
    INIT_LIST_HEAD(&plugin->list);
    INIT_LIST_HEAD(&plugin->type_list);
    INIT_LIST_HEAD(&plugin->node);
    INIT_DELAYED_WORK(&plugin->unload_work, NULL);

    /* 调用插件初始化 */
    int ret = ops->init();
    if (ret) {
        pr_err("%s: plugin '%s' init failed: %d\n",
               DRV_NAME, ops->name, ret);
        kfree(plugin);
        return ret;
    }

    /* 加入全局链表 */
    mutex_lock(&plugins_lock);
    list_add(&plugin->list, &all_plugins);
    type_idx = __builtin_ctz(plugin->type);
    if (type_idx < 8)
        list_add(&plugin->type_list, &plugins_by_type[type_idx]);
    mutex_unlock(&plugins_lock);

    pr_info("%s: registered plugin '%s' type=0x%x\n",
            DRV_NAME, ops->name, plugin->type);

    /* 发送事件 */
    {
        struct ai_plugin_event evt = {
            .event_type = AI_EVENT_PLUGIN_LOAD,
            .plugin_type = plugin->type,
        };
        ai_plugin_notify_event(&evt);
    }

    return 0;
}
EXPORT_SYMBOL_GPL(ai_plugin_register);

int ai_plugin_unregister(const char *name)
{
    struct ai_plugin *plugin, *tmp;
    int ret = -ENOENT;
    int max_wait = 1000; /* 最多等待 1 秒 */

    if (!name)
        return -EINVAL;

    mutex_lock(&plugins_lock);
    list_for_each_entry_safe(plugin, tmp, &all_plugins, list) {
        if (strncmp(plugin->name, name, AI_PLUGIN_NAME_LEN) == 0) {
            /* 等待所有活跃推理完成 */
            while (atomic_read(&plugin->active_inferences) > 0 && max_wait-- > 0) {
                mutex_unlock(&plugins_lock);
                msleep(10);
                mutex_lock(&plugins_lock);
            }

            if (refcount_read(&plugin->refcnt) > 1) {
                ret = -EBUSY;
                break;
            }

            list_del(&plugin->list);
            list_del(&plugin->type_list);
            mutex_unlock(&plugins_lock);

            /* 调用退出 */
            if (plugin->ops && plugin->ops->exit)
                plugin->ops->exit();

            /* 更新统计 */
            g_total_inferences += plugin->total_inferences;
            g_total_errors    += plugin->total_errors;
            g_total_latency_ns += plugin->total_latency_ns;

            /* 发送事件 */
            {
                struct ai_plugin_event evt = {
                    .event_type = AI_EVENT_PLUGIN_UNLOAD,
                    .plugin_type = plugin->type,
                };
                ai_plugin_notify_event(&evt);
            }

            kfree(plugin);
            pr_info("%s: unregistered plugin '%s'\n", DRV_NAME, name);
            return 0;
        }
    }
    mutex_unlock(&plugins_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(ai_plugin_unregister);

/* =========================================================================
 * 插件查找
 * ========================================================================= */

struct ai_plugin *ai_plugin_get(const char *name)
{
    struct ai_plugin *plugin;

    if (!name)
        return NULL;

    rcu_read_lock();
    list_for_each_entry_rcu(plugin, &all_plugins, list) {
        if (strncmp(plugin->name, name, AI_PLUGIN_NAME_LEN) == 0) {
            if (refcount_inc_not_zero(&plugin->refcnt)) {
                rcu_read_unlock();
                return plugin;
            }
        }
    }
    rcu_read_unlock();
    return NULL;
}
EXPORT_SYMBOL_GPL(ai_plugin_get);

void ai_plugin_put(struct ai_plugin *plugin)
{
    if (!plugin)
        return;
    if (refcount_dec_and_test(&plugin->refcnt)) {
        kfree(plugin);
    }
}
EXPORT_SYMBOL_GPL(ai_plugin_put);

struct ai_plugin *ai_plugin_first(__u32 type)
{
    struct ai_plugin *plugin;
    int idx = __builtin_ctz(type & PLUGIN_TYPE_MASK);

    rcu_read_lock();
    if (idx < 8)
        plugin = list_first_or_null_rcu(&plugins_by_type[idx],
                                        struct ai_plugin, type_list);
    else
        plugin = NULL;
    rcu_read_unlock();

    if (plugin)
        refcount_inc(&plugin->refcnt);
    return plugin;
}

struct ai_plugin *ai_plugin_next(struct ai_plugin *prev, __u32 type)
{
    struct ai_plugin *next;
    int idx = __builtin_ctz(type & PLUGIN_TYPE_MASK);

    if (!prev || idx >= 8)
        return NULL;

    rcu_read_lock();
    next = list_entry_rcu(prev->type_list.next,
                          struct ai_plugin, type_list);
    if (&next->type_list == &plugins_by_type[idx])
        next = NULL;
    rcu_read_unlock();

    if (next)
        refcount_inc(&next->refcnt);
    return next;
}

/* =========================================================================
 * 启用 / 禁用
 * ========================================================================= */

int ai_plugin_enable(const char *name)
{
    struct ai_plugin *plugin = ai_plugin_get(name);
    if (!plugin)
        return -ENOENT;

    if (atomic_read(&plugin->enabled)) {
        ai_plugin_put(plugin);
        return 0; /* 已经启用 */
    }

    int ret = 0;
    if (plugin->ops && plugin->ops->enable)
        ret = plugin->ops->enable();

    if (ret == 0) {
        atomic_set(&plugin->enabled, 1);
        plugin->flags |= PLUGIN_F_ENABLED;

        struct ai_plugin_event evt = {
            .event_type = AI_EVENT_PLUGIN_ENABLE,
            .plugin_type = plugin->type,
        };
        ai_plugin_notify_event(&evt);

        pr_info("%s: enabled plugin '%s'\n", DRV_NAME, name);
    } else {
        pr_err("%s: enable '%s' failed: %d\n", DRV_NAME, name, ret);
    }

    ai_plugin_put(plugin);
    return ret;
}
EXPORT_SYMBOL_GPL(ai_plugin_enable);

int ai_plugin_disable(const char *name)
{
    struct ai_plugin *plugin = ai_plugin_get(name);
    if (!plugin)
        return -ENOENT;

    if (!atomic_read(&plugin->enabled)) {
        ai_plugin_put(plugin);
        return 0;
    }

    int ret = 0;
    if (plugin->ops && plugin->ops->disable)
        ret = plugin->ops->disable();

    atomic_set(&plugin->enabled, 0);
    plugin->flags &= ~PLUGIN_F_ENABLED;

    struct ai_plugin_event evt = {
        .event_type = AI_EVENT_PLUGIN_DISABLE,
        .plugin_type = plugin->type,
    };
    ai_plugin_notify_event(&evt);

    pr_info("%s: disabled plugin '%s'\n", DRV_NAME, name);
    ai_plugin_put(plugin);
    return ret;
}
EXPORT_SYMBOL_GPL(ai_plugin_disable);

/* =========================================================================
 * 推理分发
 * ========================================================================= */

/*
 * ai_plugin_dispatch_infer — 分发推理到匹配的插件
 *
 * 分发策略：按优先级顺序尝试，直到成功
 */
int ai_plugin_dispatch_infer(struct ai_infer_ctx *ctx)
{
    struct ai_plugin *plugin;
    __u32 domain = ctx->domain;
    int ret = -ENODEV;
    u64 start_ns = ktime_get_ns();

    if (!ctx)
        return -EINVAL;

    /* 根据 domain 找对应类型的插件 */
    __u32 type = 0;
    switch (domain) {
    case 0: type = PLUGIN_TYPE_SCHED;     break;
    case 1: type = PLUGIN_TYPE_IO;         break;
    case 2: type = PLUGIN_TYPE_SECURITY;   break;
    case 3: type = PLUGIN_TYPE_MEMORY;    break;
    default: type = PLUGIN_TYPE_BACKEND;   break;
    }

    rcu_read_lock();
    list_for_each_entry_rcu(plugin, &all_plugins, list) {
        if (!(plugin->type & type))
            continue;
        if (!atomic_read(&plugin->enabled))
            continue;

        /* 调用插件推理 */
        if (plugin->ops && plugin->ops->infer) {
            atomic_inc(&plugin->active_inferences);

            ret = plugin->ops->infer(ctx);

            atomic_dec(&plugin->active_inferences);

            if (ret == 0) {
                /* 更新统计 */
                u64 latency = ktime_get_ns() - start_ns;
                plugin->total_inferences++;
                plugin->total_latency_ns += latency;
                g_total_inferences++;
                g_total_latency_ns += latency;

                /* 发送完成事件 */
                struct ai_plugin_event evt = {
                    .event_type = AI_EVENT_INFERENCE_DONE,
                    .plugin_type = type,
                    .data = ctx,
                };
                ai_plugin_notify_event(&evt);

                rcu_read_unlock();
                return 0;
            } else {
                plugin->total_errors++;
                g_total_errors++;
            }
        }
    }
    rcu_read_unlock();

    return ret;
}
EXPORT_SYMBOL_GPL(ai_plugin_dispatch_infer);

/* =========================================================================
 * 事件通知
 * ========================================================================= */

void ai_plugin_notify_event(struct ai_plugin_event *event)
{
    unsigned long flags;
    int i;

    if (!event)
        return;

    spin_lock_irqsave(&subscriber_lock, flags);
    for (i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (subscribers[i].valid &&
            subscribers[i].event_type == event->event_type) {
            subscribers[i].cb(event, subscribers[i].data);
        }
    }
    spin_unlock_irqrestore(&subscriber_lock, flags);
}
EXPORT_SYMBOL_GPL(ai_plugin_notify_event);

int ai_plugin_subscribe(__u32 event_type, ai_plugin_event_cb cb, void *data)
{
    unsigned long flags;
    int i;

    if (!cb)
        return -EINVAL;

    spin_lock_irqsave(&subscriber_lock, flags);
    for (i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!subscribers[i].valid) {
            subscribers[i].event_type = event_type;
            subscribers[i].cb = cb;
            subscribers[i].data = data;
            subscribers[i].valid = 1;
            spin_unlock_irqrestore(&subscriber_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&subscriber_lock, flags);
    return -ENOBUFS;
}
EXPORT_SYMBOL_GPL(ai_plugin_subscribe);

int ai_plugin_unsubscribe(__u32 event_type, ai_plugin_event_cb cb)
{
    unsigned long flags;
    int i;

    spin_lock_irqsave(&subscriber_lock, flags);
    for (i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (subscribers[i].valid &&
            subscribers[i].event_type == event_type &&
            subscribers[i].cb == cb) {
            subscribers[i].valid = 0;
            spin_unlock_irqrestore(&subscriber_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&subscriber_lock, flags);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(ai_plugin_unsubscribe);

/* =========================================================================
 * 配置
 * ========================================================================= */

int ai_plugin_set_config(const char *name, const char *key,
                         const void *val, size_t val_size)
{
    struct ai_plugin *plugin = ai_plugin_get(name);
    if (!plugin)
        return -ENOENT;

    int ret = -ENOMETHOD;
    if (plugin->ops && plugin->ops->set_param)
        ret = plugin->ops->set_param(key, val, val_size);

    ai_plugin_put(plugin);
    return ret;
}
EXPORT_SYMBOL_GPL(ai_plugin_set_config);

int ai_plugin_get_config(const char *name, const char *key,
                        void *val, size_t *val_size)
{
    struct ai_plugin *plugin = ai_plugin_get(name);
    if (!plugin)
        return -ENOENT;

    int ret = -ENOMETHOD;
    if (plugin->ops && plugin->ops->get_param)
        ret = plugin->ops->get_param(key, val, val_size);

    ai_plugin_put(plugin);
    return ret;
}
EXPORT_SYMBOL_GPL(ai_plugin_get_config);

/* =========================================================================
 * 统计
 * ========================================================================= */

void ai_plugin_stats_all(__u64 *total_inf, __u64 *total_err, __u64 *total_lat)
{
    if (total_inf) *total_inf = g_total_inferences;
    if (total_err) *total_err = g_total_errors;
    if (total_lat) *total_lat = g_total_latency_ns;
}
EXPORT_SYMBOL_GPL(ai_plugin_stats_all);

/* =========================================================================
 * proc 接口
 * ========================================================================= */

static int plugins_show(struct seq_file *m, void *v)
{
    struct ai_plugin *p;

    seq_printf(m, "AI Linux Plugin Manager v%s\n", DRV_VER);
    seq_printf(m, "==========================\n\n");
    seq_printf(m, "total_inferences: %llu\n", g_total_inferences);
    seq_printf(m, "total_errors:    %llu\n", g_total_errors);
    if (g_total_inferences > 0)
        seq_printf(m, "avg_latency_ns:  %llu\n",
                   g_total_latency_ns / g_total_inferences);
    seq_printf(m, "\n");

    seq_printf(m, "%-20s %-10s %-8s %-8s %-12s %s\n",
               "Name", "Type", "Enabled", "Active", "Inferences", "Description");
    seq_printf(m, "%-20s %-10s %-8s %-8s %-12s %s\n",
               "----", "----", "-------", "------", "-----------", "-----------");

    rcu_read_lock();
    list_for_each_entry_rcu(p, &all_plugins, list) {
        const char *type_str = "unknown";
        switch (p->type) {
        case PLUGIN_TYPE_SCHED:     type_str = "sched";     break;
        case PLUGIN_TYPE_IO:       type_str = "io";        break;
        case PLUGIN_TYPE_SECURITY:type_str = "security";  break;
        case PLUGIN_TYPE_MEMORY:  type_str = "memory";   break;
        case PLUGIN_TYPE_BACKEND:  type_str = "backend";  break;
        case PLUGIN_TYPE_FILTER:   type_str = "filter";   break;
        case PLUGIN_TYPE_ORCHESTRATOR: type_str = "orch"; break;
        }

        seq_printf(m, "%-20s %-10s %-8s %-8d %-12llu %s\n",
                   p->name,
                   type_str,
                   atomic_read(&p->enabled) ? "yes" : "no",
                   atomic_read(&p->active_inferences),
                   p->total_inferences,
                   p->ops ? (p->ops->description ? : "") : "");
    }
    rcu_read_unlock();

    return 0;
}

static int plugins_open(struct inode *inode, struct file *file)
{
    return single_open(file, plugins_show, NULL);
}

static ssize_t plugins_write(struct file *file, const char __user *buf,
                           size_t count, loff_t *ppos)
{
    char kbuf[256];

    if (count >= sizeof(kbuf))
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    kbuf[count] = '\0';
    strim(kbuf);

    if (str_has_prefix(kbuf, "enable ")) {
        char *name = kbuf + 7;
        strim(name);
        int ret = ai_plugin_enable(name);
        pr_info("%s: enable '%s' -> %d\n", DRV_NAME, name, ret);
        return count;
    }

    if (str_has_prefix(kbuf, "disable ")) {
        char *name = kbuf + 8;
        strim(name);
        int ret = ai_plugin_disable(name);
        pr_info("%s: disable '%s' -> %d\n", DRV_NAME, name, ret);
        return count;
    }

    return count;
}

static const struct proc_ops plugins_proc_fops = {
    .proc_open    = plugins_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
    .proc_write   = plugins_write,
};

void ai_proc_plugins_init(void)
{
    ai_plugins_dir = proc_mkdir("ai_plugins", NULL);
    if (ai_plugins_dir)
        proc_create("list", 0600, ai_plugins_dir, &plugins_proc_fops);
}

void ai_proc_plugins_exit(void)
{
    remove_proc_subtree("ai_plugins", NULL);
}

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init ai_plugin_mgr_init(void)
{
    pr_info("========================================\n");
    pr_info("  AI Linux Plugin Manager v%s\n", DRV_VER);
    pr_info("  插件系统就绪\n");
    pr_info("========================================\n");

    ai_proc_plugins_init();

    pr_info("%s: ready\n", DRV_NAME);
    return 0;
}

static void __exit ai_plugin_mgr_exit(void)
{
    struct ai_plugin *p, *tmp;

    pr_info("%s: shutting down\n", DRV_NAME);

    /* 卸载所有插件 */
    mutex_lock(&plugins_lock);
    list_for_each_entry_safe(p, tmp, &all_plugins, list) {
        if (p->ops && p->ops->exit)
            p->ops->exit();
        list_del(&p->list);
        list_del(&p->type_list);
        kfree(p);
    }
    mutex_unlock(&plugins_lock);

    ai_proc_plugins_exit();

    pr_info("%s: stopped\n", DRV_NAME);
}

module_init(ai_plugin_mgr_init);
module_exit(ai_plugin_mgr_exit);

MODULE_DESCRIPTION("AI Linux Plugin Manager");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
