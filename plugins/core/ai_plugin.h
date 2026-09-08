/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ai_plugin.h — AI Linux 插件核心接口
 *
 * 所有插件必须实现 ai_plugin_ops 结构体，并注册到插件管理器。
 *
 * 插件类型：
 *   - PLUGIN_TYPE_SCHED     调度插件
 *   - PLUGIN_TYPE_IO        IO 插件
 *   - PLUGIN_TYPE_SECURITY  安全插件
 *   - PLUGIN_TYPE_MEMORY   内存插件
 *   - PLUGIN_TYPE_BACKEND  后端插件（DeepSeek/Kimi/自定义）
 *   - PLUGIN_TYPE_FILTER   输入/输出过滤器
 *   - PLUGIN_TYPE_ORCHESTRATOR 编排器
 */

#ifndef _AI_PLUGIN_H
#define _AI_PLUGIN_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/refcount.h>
#include <linux/workqueue.h>

#define AI_PLUGIN_VERSION   0x00010000  /* v1.0.0 */
#define AI_PLUGIN_NAME_LEN  64
#define AI_PLUGIN_DESC_LEN 256
#define AI_PLUGIN_PATH_LEN 512

/* 插件类型 */
#define PLUGIN_TYPE_MASK      0xFF
#define PLUGIN_TYPE_SCHED     0x01
#define PLUGIN_TYPE_IO        0x02
#define PLUGIN_TYPE_SECURITY  0x04
#define PLUGIN_TYPE_MEMORY   0x08
#define PLUGIN_TYPE_BACKEND  0x10
#define PLUGIN_TYPE_FILTER   0x20
#define PLUGIN_TYPE_ORCHESTRATOR 0x40

/* 插件标志 */
#define PLUGIN_F_ENABLED      0x01
#define PLUGIN_F_LOADED      0x02
#define PLUGIN_F_AUTOLOAD    0x04  /* 随系统自动加载 */
#define PLUGIN_F_PRIVILEGED  0x08  /* 需要 CAP_SYS_ADMIN */
#define PLUGIN_F_ASYNC       0x10  /* 异步执行 */

/* 推理上下文 */
struct ai_infer_ctx {
    __u64  request_id;
    __u32  domain;            /* 决策域 */
    __u32  flags;
    void  *input;
    size_t  input_size;
    void  *output;
    size_t  output_size;
    void  *priv;             /* 插件私有数据 */

    /* 元信息 */
    char   model_name[64];
    char   backend[32];
    void  *session;           /* 推理会话 */
    __u64  timestamp_ns;

    /* 回调 */
    void (*done)(struct ai_infer_ctx *, int err, void *result);
};

/* 插件操作接口 */
struct ai_plugin_ops {
    /* 必需 */
    const char *name;
    const char *version;
    const char *description;
    __u32       type;           /* PLUGIN_TYPE_* */
    __u32       api_version;   /* 必须等于 AI_PLUGIN_VERSION */

    /* 生命周期 */
    int  (*init)(void);            /* 插件初始化 */
    void (*exit)(void);            /* 插件退出 */
    int  (*enable)(void);           /* 启用插件 */
    int  (*disable)(void);          /* 禁用插件 */

    /* 推理入口（主要）*/
    int  (*infer)(struct ai_infer_ctx *ctx);

    /* 可选扩展 */
    int  (*infer_async)(struct ai_infer_ctx *ctx);
    int  (*infer_cancel)(__u64 request_id);

    /* 过滤器（用于输入/输出后处理）*/
    int  (*preprocess)(void *input, size_t in_size,
                       void **out, size_t *out_size);
    int  (*postprocess)(void *input, size_t in_size,
                        void *output, size_t out_size,
                        void **result, size_t *result_size);

    /* 配置 */
    int  (*set_param)(const char *key, const void *val, size_t val_size);
    int  (*get_param)(const char *key, void *val, size_t *val_size);

    /* 统计 */
    void (*get_stats)(__u64 *inferences, __u64 *errors,
                        __u64 *total_latency_ns);

    /* 私有数据 */
    void *priv;
};

/* 插件描述符 */
struct ai_plugin {
    const struct ai_plugin_ops *ops;

    /* 元数据 */
    char     name[AI_PLUGIN_NAME_LEN];
    char     path[AI_PLUGIN_PATH_LEN];
    __u32    type;
    __u32    flags;
    refcount_t refcnt;

    /* 状态 */
    atomic_t enabled;
    atomic_t busy;

    /* 插件链 */
    struct list_head list;           /* 全局插件链表 */
    struct list_head type_list;       /* 按类型组织的链表 */
    struct list_head node;           /* 用于类型链 */
    struct delayed_work unload_work;  /* 延迟卸载 */

    /* 统计 */
    __u64  total_inferences;
    __u64  total_errors;
    __u64  total_latency_ns;
    atomic_t active_inferences;

    /* 权限 */
    void   *module;               /* 所属内核模块 */
};

/* 插件事件 */
struct ai_plugin_event {
    __u32  event_type;
    __u32  plugin_type;
    void  *data;
    size_t data_size;
    void  (*ack)(struct ai_plugin_event *, int result);
    void  *priv;
};

/* 事件类型 */
#define AI_EVENT_PLUGIN_LOAD     1
#define AI_EVENT_PLUGIN_UNLOAD   2
#define AI_EVENT_PLUGIN_ENABLE   3
#define AI_EVENT_PLUGIN_DISABLE  4
#define AI_EVENT_INFERENCE_START 10
#define AI_EVENT_INFERENCE_DONE  11
#define AI_EVENT_INFERENCE_ERROR 12
#define AI_EVENT_ROUTING        20

/* 插件管理器回调 */
typedef void (*ai_plugin_event_cb)(struct ai_plugin_event *event, void *data);

/* =========================================================================
 * 插件管理器 API
 * ========================================================================= */

/* 注册 / 注销插件 */
int ai_plugin_register(struct ai_plugin_ops *ops);
int ai_plugin_unregister(const char *name);

/* 查找插件 */
struct ai_plugin *ai_plugin_get(const char *name);
void ai_plugin_put(struct ai_plugin *plugin);
struct ai_plugin *ai_plugin_first(__u32 type);
struct ai_plugin *ai_plugin_next(struct ai_plugin *prev, __u32 type);

/* 启用 / 禁用 */
int ai_plugin_enable(const char *name);
int ai_plugin_disable(const char *name);
int ai_plugin_set_autoload(const char *name, int autoload);

/* 推理分发（遍历所有匹配类型的插件）*/
int ai_plugin_dispatch_infer(struct ai_infer_ctx *ctx);
int ai_plugin_dispatch_infer_async(struct ai_infer_ctx *ctx);

/* 事件订阅 */
int ai_plugin_subscribe(__u32 event_type, ai_plugin_event_cb cb, void *data);
int ai_plugin_unsubscribe(__u32 event_type, ai_plugin_event_cb cb);

/* 配置传递 */
int ai_plugin_set_config(const char *name, const char *key,
                        const void *val, size_t val_size);
int ai_plugin_get_config(const char *name, const char *key,
                        void *val, size_t *val_size);

/* 统计 */
void ai_plugin_stats_all(__u64 *total_inf, __u64 *total_err, __u64 *total_lat);

/* proc 接口 */
void ai_proc_plugins_init(void);
void ai_proc_plugins_exit(void);

/* =========================================================================
 * 插件模块化宏（简化注册）
 * ========================================================================= */

/*
 * AI_PLUGIN_REGISTER — 注册插件
 *
 * 使用方法：
 *   AI_PLUGIN_REGISTER(my_plugin, sched_infer_ops, "sched_deepseek");
 */
#define AI_PLUGIN_REGISTER(_name, _ops, _path) \
    static int __init _name##_init(void) { \
        strscpy(((_ops)->name ? : ""), _name, AI_PLUGIN_NAME_LEN-1); \
        return ai_plugin_register(_ops); \
    } \
    static void __exit _name##_exit(void) { \
        ai_plugin_unregister(_name); \
    }

#endif /* _AI_PLUGIN_H */
