/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ai_core.h — AI Linux 核心子系统公共头文件
 *
 * 定义所有内核 AI 组件共享的数据结构、API 和常量。
 */

#ifndef _AI_CORE_H
#define _AI_CORE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/refcount.h>
#include <linux/rwlock_types.h>
#include <linux/workqueue.h>

#define AI_MODEL_NAME_LEN   64
#define AI_MAX_FEATURES     256
#define AI_VERSION          "1.0.0"

/* ---------------------------------------------------------------------------
 * 推理方向：同步或异步
 * --------------------------------------------------------------------------- */
enum ai_infer_mode {
    AI_INFER_SYNC   = 0,   /* 阻塞，实时决策用（调度器路径） */
    AI_INFER_ASYNC  = 1,   /* 通过 workqueue，不阻塞 */
};

/* ---------------------------------------------------------------------------
 * 推理结果评分类型（0.0 ~ 1.0）
 * --------------------------------------------------------------------------- */
typedef struct {
    __u8  label;      /* 分类标签 */
    __u8  confidence; /* 置信度 × 100 */
} ai_class_t;

typedef struct {
    float          score;   /* 回归分数 */
    ai_class_t     top_class;
} ai_result_t;

/* ---------------------------------------------------------------------------
 * 推理请求
 * --------------------------------------------------------------------------- */
struct ai_inference_request {
    const char    *model_name;
    void          *input;       /* 用户态输入（由调用者保证生命周期） */
    size_t         input_size;
    void          *output;      /* 输出缓冲区 */
    size_t         output_size;
    enum ai_infer_mode mode;

    /* 回调（仅异步模式） */
    void (*done)(struct ai_inference_result *, void *);
    void  *priv;

    struct work_struct work;    /* 异步工作项 */
};

/* ---------------------------------------------------------------------------
 * 推理结果
 * --------------------------------------------------------------------------- */
struct ai_inference_result {
    int      err;
    void    *output;
    size_t   output_size;
    u64      latency_ns;
    ai_result_t result;
    void    *priv;
};

/* ---------------------------------------------------------------------------
 * 模型描述
 * --------------------------------------------------------------------------- */
struct ai_model {
    char             name[AI_MODEL_NAME_LEN];
    int              version;
    void            *weights;        /* mmap 权重区域，PG_reserved 保护 */
    size_t           weight_size;
    refcount_t       refcnt;
    rwlock_t         lock;          /* 保护模型内容 */

    /* 模型元数据 */
    int              num_classes;
    int              input_dim;
    int              output_dim;
    int              is_quantized;   /* 是否量化模型 */
    const char      *backend;        /* "soft" / "npu" / "gpu" */

    struct list_head list;           /* 全局模型链表 */
    struct list_head node;           /* 统计链表 */
};

typedef struct ai_model ai_model_t;

/* ---------------------------------------------------------------------------
 * 任务特征（用于调度器等子系统上报数据）
 * --------------------------------------------------------------------------- */
struct ai_task_features {
    __u64  sum_exec_runtime;   /* CPU 运行总时间（ns） */
    __u32  nvcsw;              /* 自愿上下文切换 */
    __u32  nivcsw;             /* 非自愿上下文切换 */
    __u32  cpu_util;           /* CPU 利用率（0-1024） */
    __u32  io_wait_ns;         /* IO 等待时间（ns） */
    __u32  cache_miss_rate;    /* 缓存未命中率（0-1000） */
    __u32  mem_usage_kb;       /* 内存使用量（KB） */
    __u32  numa_node;
    __u8   prio;               /* 调度优先级 */
    __u8   oom_score_adj;
    char   comm[16];           /* 进程名 */
};

/* ---------------------------------------------------------------------------
 * AI 调度决策
 * --------------------------------------------------------------------------- */
enum ai_sched_decision {
    AI_KEEP     = 0,
    AI_PROMOTE  = 1,   /* 升权，多分时间片 */
    AI_DEMOTE   = 2,   /* 降权，少分时间片 */
    AI_MIGRATE  = 3,   /* 迁移到其他 CPU */
    AI_BATCH    = 4,   /* 标记为批处理任务 */
    AI_IDLE     = 5,   /* 插入 idle */
};

/* ---------------------------------------------------------------------------
 * 推理统计
 * --------------------------------------------------------------------------- */
struct ai_stats {
    u64 total_inferences;
    u64 sync_inferences;
    u64 async_inferences;
    u64 sync_total_ns;
    u64 async_total_ns;
    u64 errors;
    u64 oom_errors;
    u64 cache_hits;
    u64 cache_misses;
    atomic_t busy;   /* 推理引擎正忙 */
};

/* ---------------------------------------------------------------------------
 * AI 加速器设备类型
 * --------------------------------------------------------------------------- */
enum ai_vendor {
    AI_VENDOR_SOFT        = 0,  /* 软件模拟 */
    AI_VENDOR_NVIDIA      = 1,
    AI_VENDOR_AMD         = 2,
    AI_VENDOR_INTEL_MLA   = 3,
    AI_VENDOR_HISI_ASCEND = 4,
    AI_VENDOR_CAMBRICON   = 5,
    AI_VENDOR_KUNLUN      = 6,
    AI_VENDOR_CUSTOM      = 99,
};

/* ---------------------------------------------------------------------------
 * 设备操作接口
 * --------------------------------------------------------------------------- */
struct ai_device;
struct ai_hw_ops {
    int  (*run)(struct ai_device *dev, const void *in, size_t isize,
                void *out, size_t osize);
    int  (*submit)(struct ai_device *dev, struct ai_inference_request *req);
    void (*sync)(struct ai_device *dev);
    int  (*get_result)(struct ai_device *dev, void *out, size_t osize);
    void (*release)(struct ai_device *dev);
};

struct ai_device {
    char                name[64];
    enum ai_vendor      vendor;
    struct ai_hw_ops   *ops;
    void               *priv;
    refcount_t          refcnt;
    struct list_head    list;
};

/* ---------------------------------------------------------------------------
 * 公共 API（ai_core 模块导出）
 * --------------------------------------------------------------------------- */

/* 模型管理 */
ai_model_t *ai_model_register(const char *name, int version,
                               void *weights, size_t weight_size,
                               int num_classes, int input_dim, int output_dim);
void         ai_model_put(ai_model_t *model);
ai_model_t  *ai_model_get(const char *name);
int          ai_model_unregister(const char *name);

/* 同步推理（推荐用于调度等短路径） */
int ai_infer_sync(const char *model_name,
                  const void *input, size_t isize,
                  void *output, size_t osize,
                  u64 *latency_ns);

/* 异步推理 */
int ai_infer_async(struct ai_inference_request *req);

/* 便捷封装：调度决策推理 */
enum ai_sched_decision
ai_infer_sched_decision(const struct ai_task_features *f);

/* 便捷封装：IO 异常分推理 */
float ai_infer_io_score(const void *packet, size_t size);

/* 便捷封装：安全异常分推理 */
float ai_infer_security_score(const char *comm, const struct ai_task_features *f);

/* 统计 */
void ai_stats_read(struct ai_stats *out);
void ai_stats_reset(void);

/* 设备管理 */
struct ai_device *ai_register_device(const char *name, enum ai_vendor vendor,
                                     struct ai_hw_ops *ops, void *priv);
void              ai_put_device(struct ai_device *dev);

/* proc 接口 */
void ai_proc_init(void);
void ai_proc_exit(void);

/* 模块信息 */
const char *ai_version_string(void);
int          ai_self_check(void);  /* 自我检测 */

/* ---------------------------------------------------------------------------
 * 系统调用号（由 arch 提供，当前使用通用号）
 * --------------------------------------------------------------------------- */
#ifndef __NR_ai_infer
#define __NR_ai_infer 548  /* 待 arch/x86/entry/syscalls/ 分配后确认 */
#endif

/*
 * sys_ai_infer — 推理系统调用入口
 *
 * arg0: model_name (const char __user *)
 * arg1: input      (void __user *)
 * arg2: input_size (size_t)
 * arg3: output     (void __user *)
 * arg4: output_size(size_t)
 */
long sys_ai_infer(unsigned long arg0, unsigned long arg1,
                  unsigned long arg2, unsigned long arg3,
                  unsigned long arg4);

/* ---------------------------------------------------------------------------
 * debugfs 接口
 * --------------------------------------------------------------------------- */
void ai_debugfs_init(void);
void ai_debugfs_exit(void);

/* ---------------------------------------------------------------------------
 * 模型注册宏（供具体模型模块使用）
 * --------------------------------------------------------------------------- */
#define module_ai_model(_name, _ver, _weights, _size, _nc, _id, _od) \
    static ai_model_t *ai_model_##_name##_ptr __used \
    __attribute__((section(".ai_models"), aligned(sizeof(void *)))) \
    = NULL /* 由 ai_core 模块在 init 时填充 */

#endif /* _AI_CORE_H */
