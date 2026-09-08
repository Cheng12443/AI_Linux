// SPDX-License-Identifier: GPL-2.0
/*
 * ai_core.c — AI Linux 核心子系统
 *
 * 提供：
 *  - 模型注册与管理
 *  - 同步/异步推理接口
 *  - AI 调度决策封装
 *  - procfs / debugfs 统计接口
 *  - sys_infer() 系统调用
 *
 * 作者：AI Linux Team
 * 版本：1.0.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/rwlock.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/sysctl.h>
#include <linux/ktime.h>

#include "ai_core.h"

#define DRV_NAME  "ai_core"
#define DRV_VER   "1.0.0"

/* =========================================================================
 * 全局变量
 * ========================================================================= */

static LIST_HEAD(ai_models);       /* 注册的模型链表 */
static LIST_HEAD(ai_devices);      /* 注册的 AI 设备链表 */
static DEFINE_RWLOCK(ai_lock);     /* 保护模型和设备列表 */

static struct ai_stats global_stats;
static atomic_t inference_seq = ATOMIC_INIT(0);

/* proc 接口 */
static struct proc_dir_entry *ai_proc_dir;
static struct dentry *ai_debugfs_dir;

/* 工作队列（用于异步推理） */
static struct workqueue_struct *ai_wq;

/* 推理引擎选择：0=软件模拟，1=硬件加速 */
static int ai_engine_mode = 0;
module_param(ai_engine_mode, int, 0644);
MODULE_PARM_DESC(ai_engine_mode, "0=soft, 1=hw_accelerated");

static bool ai_enabled = true;
module_param(ai_enabled, bool, 0644);
MODULE_PARM_DESC(ai_enabled, "enable/disable AI inference");

/* 调度决策阈值 */
static int sched_promote_threshold  = 70;  /* >70 分 → 升权 */
static int sched_demote_threshold   = 30;  /* <30 分 → 降权 */
static int sched_migrate_threshold  = 60;  /* >60 分 → 考虑迁移 */

module_param(sched_promote_threshold, int, 0644);
module_param(sched_demote_threshold,  int, 0644);
module_param(sched_migrate_threshold, int, 0644);

/* sysctl 接口 */
static struct ctl_table ai_sysctl_table[] = {
    {
        .procname = "ai_enabled",
        .data     = &ai_enabled,
        .maxlen   = sizeof(ai_enabled),
        .mode     = 0644,
        .proc_handler = proc_dobool,
    },
    {
        .procname = "engine_mode",
        .data     = &ai_engine_mode,
        .maxlen   = sizeof(ai_engine_mode),
        .mode     = 0644,
        .proc_handler = proc_dointvec_minmax,
        .extra1   = SYSCTL_ZERO,
        .extra2   = SYSCTL_ONE,
    },
    {
        .procname = "sched_promote_threshold",
        .data     = &sched_promote_threshold,
        .maxlen   = sizeof(int),
        .mode     = 0644,
        .proc_handler = proc_dointvec_minmax,
        .extra1   = SYSCTL_ZERO,
        .extra2   = SYSCTL_ONEHUNDRED,
    },
    {
        .procname = "sched_demote_threshold",
        .data     = &sched_demote_threshold,
        .maxlen   = sizeof(int),
        .mode     = 0644,
        .proc_handler = proc_dointvec_minmax,
        .extra1   = SYSCTL_ZERO,
        .extra2   = SYSCTL_ONEHUNDRED,
    },
    { }
};

static struct ctl_table ai_sysctl_root[] = {
    {
        .procname = "ai",
        .mode     = 0555,
        .child    = ai_sysctl_table,
    },
    { }
};

/* =========================================================================
 * 软件推理引擎（最简线性模型演示）
 * 真实部署时替换为 ONNX Runtime / TensorRT / TFLite 内核化版本
 * ========================================================================= */

/*
 * soft_infer — 软件推理引擎
 *
 * 这里实现一个演示性的线性模型：
 *   score = sigmoid(w * features + b)
 *
 * 真实部署时应调用:
 *   - libonnxruntime (需内核化或通过 io_uring 转发用户态)
 *   - 内核内嵌的 tinygrad
 *   - 厂商 NPU SDK
 */
static int soft_infer(const void *input, size_t isize,
                      void *output, size_t osize)
{
    /*
     * 演示：提取特征向量的第一个 32 位整数，
     * 把它归一化到 [0,1] 作为评分输出。
     * 真实模型：遍历 weight blob，应用矩阵乘法。
     */
    const __u32 *feat = (const __u32 *)input;
    size_t n = min(isize / sizeof(__u32), (size_t)AI_MAX_FEATURES);

    /* 最简加权平均作为演示分 */
    __u64 sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += feat[i];
    }

    float score = 0.0f;
    if (n > 0) {
        /* 假设特征值范围 0-1000，简单归一化 */
        score = (float)(sum / n) / 1000.0f;
        if (score > 1.0f) score = 1.0f;
    }

    /* 写回输出（至少 4 字节） */
    if (osize < sizeof(float))
        return -ENOSPC;

    *(__u32 *)output = *(__u32 *)&score;
    if (osize >= sizeof(ai_result_t)) {
        ai_result_t *r = (ai_result_t *)output;
        r->score = score;
        r->top_class.confidence = (int)(score * 100);
        r->top_class.label = (score > 0.5f) ? 1 : 0;
    }

    return 0;
}

/* =========================================================================
 * 模型管理
 * ========================================================================= */

ai_model_t *ai_model_register(const char *name, int version,
                               void *weights, size_t weight_size,
                               int num_classes, int input_dim, int output_dim)
{
    ai_model_t *model;

    if (!name || !weights || !weight_size)
        return ERR_PTR(-EINVAL);

    model = kzalloc(sizeof(ai_model_t), GFP_KERNEL);
    if (!model)
        return ERR_PTR(-ENOMEM);

    strscpy(model->name, name, AI_MODEL_NAME_LEN - 1);
    model->version    = version;
    model->weights    = weights;
    model->weight_size = weight_size;
    model->num_classes = num_classes;
    model->input_dim   = input_dim;
    model->output_dim  = output_dim;
    model->backend     = "soft";
    refcount_set(&model->refcnt, 1);
    rwlock_init(&model->lock);

    /* 将权重页标记为 reserved，防止被换出 */
    /* 真实场景：在 mmap 时处理，这里做粗略保护 */
    SetPageReserved(virt_to_page(weights));

    write_lock(&ai_lock);
    list_add(&model->list, &ai_models);
    write_unlock(&ai_lock);

    pr_info("%s: registered model '%s' v%d (%zu bytes)\n",
            DRV_NAME, name, version, weight_size);

    return model;
}
EXPORT_SYMBOL_GPL(ai_model_register);

void ai_model_put(ai_model_t *model)
{
    if (!model)
        return;

    if (refcount_dec_and_test(&model->refcnt)) {
        write_lock(&ai_lock);
        list_del(&model->list);
        write_unlock(&ai_lock);

        if (model->weights)
            ClearPageReserved(virt_to_page(model->weights));

        kfree(model);
        pr_debug("%s: model freed\n", DRV_NAME);
    }
}
EXPORT_SYMBOL_GPL(ai_model_put);

ai_model_t *ai_model_get(const char *name)
{
    ai_model_t *model, *found = NULL;

    read_lock(&ai_lock);
    list_for_each_entry(model, &ai_models, list) {
        if (strncmp(model->name, name, AI_MODEL_NAME_LEN) == 0) {
            if (refcount_inc_not_zero(&model->refcnt)) {
                found = model;
            }
            break;
        }
    }
    read_unlock(&ai_lock);

    return found ?: ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL_GPL(ai_model_get);

int ai_model_unregister(const char *name)
{
    ai_model_t *model;

    write_lock(&ai_lock);
    list_for_each_entry(model, &ai_models, list) {
        if (strncmp(model->name, name, AI_MODEL_NAME_LEN) == 0) {
            if (refcount_read(&model->refcnt) > 1) {
                write_unlock(&ai_lock);
                return -EBUSY;
            }
            list_del(&model->list);
            write_unlock(&ai_lock);
            ClearPageReserved(virt_to_page(model->weights));
            kfree(model);
            return 0;
        }
    }
    write_unlock(&ai_lock);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(ai_model_unregister);

/* =========================================================================
 * 推理接口
 * ========================================================================= */

/*
 * ai_infer_sync — 同步推理
 *
 * 用于：调度决策、IO 分类、安全检测等延迟敏感路径
 *
 * 流程：
 *   1. 查找模型
 *   2. 调用推理引擎
 *   3. 记录延迟和统计
 */
int ai_infer_sync(const char *model_name,
                  const void *input, size_t isize,
                  void *output, size_t osize,
                  u64 *latency_ns)
{
    ai_model_t *model;
    int ret;
    u64 start, end;

    if (!ai_enabled)
        return -ENODEV;

    if (!model_name || !input || !output)
        return -EINVAL;

    model = ai_model_get(model_name);
    if (IS_ERR(model))
        return PTR_ERR(model);

    atomic_inc(&inference_seq);

    start = ktime_get_ns();

    /* 根据 engine_mode 选择推理后端 */
    if (ai_engine_mode == 0) {
        ret = soft_infer(input, isize, output, osize);
    } else {
        /* 硬件加速路径：查找对应设备并调用 */
        struct ai_device *dev;
        read_lock(&ai_lock);
        list_for_each_entry(dev, &ai_devices, list) {
            if (dev->ops && dev->ops->run) {
                ret = dev->ops->run(dev, input, isize, output, osize);
                read_unlock(&ai_lock);
                goto done;
            }
        }
        read_unlock(&ai_lock);
        /* 无硬件，回退到软件 */
        ret = soft_infer(input, isize, output, osize);
    }

done:
    end = ktime_get_ns();
    if (latency_ns)
        *latency_ns = end - start;

    global_stats.total_inferences++;
    global_stats.sync_inferences++;
    global_stats.sync_total_ns += (end - start);

    if (ret)
        global_stats.errors++;

    ai_model_put(model);
    return ret;
}
EXPORT_SYMBOL_GPL(ai_infer_sync);

/* 异步推理工作项 */
static void ai_async_work(struct work_struct *work)
{
    struct ai_inference_request *req =
        container_of(work, struct ai_inference_request, work);

    struct ai_inference_result result = { 0 };
    u64 latency_ns;
    int ret;

    ret = ai_infer_sync(req->model_name, req->input, req->input_size,
                        req->output, req->output_size, &latency_ns);

    result.err = ret;
    result.latency_ns = latency_ns;
    result.priv = req->priv;

    if (ret == 0) {
        /* 从输出缓冲区提取 result */
        if (req->output_size >= sizeof(ai_result_t)) {
            memcpy(&result.result, req->output, sizeof(ai_result_t));
        }
    }

    if (req->done)
        req->done(&result, req->priv);

    /* 不释放 req，由调用者保证生命周期 */
}

int ai_infer_async(struct ai_inference_request *req)
{
    if (!ai_enabled)
        return -ENODEV;

    if (!req || !req->model_name)
        return -EINVAL;

    if (req->mode != AI_INFER_ASYNC) {
        /* 自动降级为同步 */
        return ai_infer_sync(req->model_name,
                             req->input, req->input_size,
                             req->output, req->output_size, NULL);
    }

    atomic_inc(&inference_seq);
    global_stats.async_inferences++;

    INIT_WORK(&req->work, ai_async_work);
    queue_work(ai_wq, &req->work);

    return 0;  /* 异步，调用者不要在 callback 前访问 output */
}
EXPORT_SYMBOL_GPL(ai_infer_async);

/* =========================================================================
 * 便捷推理封装
 * ========================================================================= */

/*
 * ai_infer_sched_decision — 调度决策推理
 *
 * 输入：任务特征
 * 输出：调度决策
 */
enum ai_sched_decision
ai_infer_sched_decision(const struct ai_task_features *f)
{
    ai_result_t result = { 0 };
    int ret;

    if (!ai_enabled)
        return AI_KEEP;

    ret = ai_infer_sync("sched_decision",
                        f, sizeof(struct ai_task_features),
                        &result, sizeof(result), NULL);
    if (ret)
        return AI_KEEP;  /* 推理失败，保持默认 */

    /* 根据模型输出分映射到决策 */
    if (result.score * 100 > sched_promote_threshold)
        return AI_PROMOTE;

    if (result.score * 100 < sched_demote_threshold)
        return AI_DEMOTE;

    if (result.score * 100 > sched_migrate_threshold) {
        /* 进一步判断是否真的需要迁移（当前 CPU 繁忙？） */
        return AI_MIGRATE;
    }

    return AI_KEEP;
}
EXPORT_SYMBOL_GPL(ai_infer_sched_decision);

/*
 * ai_infer_io_score — IO 异常评分
 */
float ai_infer_io_score(const void *packet, size_t size)
{
    float score = 0.0f;
    ai_result_t result = { 0 };
    int ret;

    if (!ai_enabled || !packet)
        return 0.0f;

    ret = ai_infer_sync("io_anomaly",
                        packet, size,
                        &result, sizeof(result), NULL);
    if (ret)
        return 0.0f;

    return result.score;
}
EXPORT_SYMBOL_GPL(ai_infer_io_score);

/*
 * ai_infer_security_score — 安全异常评分
 */
float ai_infer_security_score(const char *comm,
                               const struct ai_task_features *f)
{
    float score = 0.0f;
    ai_result_t result = { 0 };
    int ret;

    if (!ai_enabled)
        return 0.0f;

    /* 将进程名和特征打包 */
    struct {
        char comm[16];
        struct ai_task_features f;
    } __packed input;

    strscpy(input.comm, comm ? comm : "unknown", 16);
    memcpy(&input.f, f, sizeof(struct ai_task_features));

    ret = ai_infer_sync("security_anomaly",
                        &input, sizeof(input),
                        &result, sizeof(result), NULL);
    if (ret)
        return 0.0f;

    return result.score;
}
EXPORT_SYMBOL_GPL(ai_infer_security_score);

/* =========================================================================
 * 统计
 * ========================================================================= */

void ai_stats_read(struct ai_stats *out)
{
    if (!out)
        return;

    memcpy(out, &global_stats, sizeof(global_stats));
}
EXPORT_SYMBOL_GPL(ai_stats_read);

void ai_stats_reset(void)
{
    memset(&global_stats, 0, sizeof(global_stats));
    atomic_set(&inference_seq, 0);
}
EXPORT_SYMBOL_GPL(ai_stats_reset);

/* =========================================================================
 * 设备管理
 * ========================================================================= */

struct ai_device *ai_register_device(const char *name, enum ai_vendor vendor,
                                     struct ai_hw_ops *ops, void *priv)
{
    struct ai_device *dev;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return ERR_PTR(-ENOMEM);

    strscpy(dev->name, name, sizeof(dev->name) - 1);
    dev->vendor = vendor;
    dev->ops    = ops;
    dev->priv   = priv;
    refcount_set(&dev->refcnt, 1);

    write_lock(&ai_lock);
    list_add(&dev->list, &ai_devices);
    write_unlock(&ai_lock);

    pr_info("%s: registered AI device '%s' (vendor=%d)\n",
            DRV_NAME, name, vendor);

    return dev;
}
EXPORT_SYMBOL_GPL(ai_register_device);

void ai_put_device(struct ai_device *dev)
{
    if (!dev)
        return;

    if (refcount_dec_and_test(&dev->refcnt)) {
        write_lock(&ai_lock);
        list_del(&dev->list);
        write_unlock(&ai_lock);

        if (dev->ops && dev->ops->release)
            dev->ops->release(dev);

        kfree(dev);
    }
}
EXPORT_SYMBOL_GPL(ai_put_device);

/* =========================================================================
 * procfs 接口
 * ========================================================================= */

static int ai_stats_show(struct seq_file *m, void *v)
{
    struct ai_stats s;
    ai_stats_read(&s);

    seq_printf(m, "AI Linux Core v%s\n", DRV_VER);
    seq_printf(m, "===========================\n");
    seq_printf(m, "enabled:         %d\n", ai_enabled);
    seq_printf(m, "engine_mode:     %s\n", ai_engine_mode ? "hardware" : "software");
    seq_printf(m, "total_inferences:%llu\n", s.total_inferences);
    seq_printf(m, "sync:            %llu  (avg %llu ns)\n",
               s.sync_inferences,
               s.sync_inferences ? s.sync_total_ns / s.sync_inferences : 0);
    seq_printf(m, "async:           %llu  (avg %llu ns)\n",
               s.async_inferences,
               s.async_inferences ? s.async_total_ns / s.async_inferences : 0);
    seq_printf(m, "errors:          %llu\n", s.errors);
    seq_printf(m, "cache_hits:      %llu\n", s.cache_hits);
    seq_printf(m, "cache_misses:    %llu\n", s.cache_misses);
    seq_printf(m, "inference_seq:   %d\n", atomic_read(&inference_seq));

    seq_printf(m, "\nsched thresholds: promote>%d demote<%d migrate>%d\n",
               sched_promote_threshold, sched_demote_threshold, sched_migrate_threshold);

    /* 列出已注册模型 */
    {
        ai_model_t *model;
        seq_printf(m, "\nregistered models:\n");
        read_lock(&ai_lock);
        list_for_each_entry(model, &ai_models, list) {
            seq_printf(m, "  %s v%d classes=%d dim_in=%d dim_out=%d backend=%s\n",
                       model->name, model->version,
                       model->num_classes, model->input_dim, model->output_dim,
                       model->backend);
        }
        read_unlock(&ai_lock);
    }

    return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE(ai_stats);

static int ai_debug_show(struct seq_file *m, void *v)
{
    seq_printf(m, "AI Linux debug interface\n");
    seq_printf(m, "build: %s %s\n", __DATE__, __TIME__);
    return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE(ai_debug);

void ai_proc_init(void)
{
    ai_proc_dir = proc_mkdir("ai", NULL);
    if (!ai_proc_dir)
        return;

    proc_create("stats", 0444, ai_proc_dir, &ai_stats_proc_fops);
    proc_create("debug", 0444, ai_proc_dir, &ai_debug_proc_fops);
}

void ai_proc_exit(void)
{
    remove_proc_subtree("ai", NULL);
}

/* =========================================================================
 * debugfs 接口
 * ========================================================================= */

void ai_debugfs_init(void)
{
    ai_debugfs_dir = debugfs_create_dir("ai", NULL);
    if (!ai_debugfs_dir)
        return;

    debugfs_create_bool("enabled", 0644, ai_debugfs_dir, &ai_enabled);
    debugfs_create_u32("engine_mode", 0644, ai_debugfs_dir,
                       (u32 *)&ai_engine_mode);
    debugfs_create_u32("sched_promote_thresh", 0644, ai_debugfs_dir,
                       (u32 *)&sched_promote_threshold);
    debugfs_create_u32("sched_demote_thresh", 0644, ai_debugfs_dir,
                       (u32 *)&sched_demote_threshold);
}

void ai_debugfs_exit(void)
{
    debugfs_remove_recursive(ai_debugfs_dir);
}

/* =========================================================================
 * sys_ai_infer — 推理系统调用
 * ========================================================================= */

long sys_ai_infer(unsigned long arg0, unsigned long arg1,
                  unsigned long arg2, unsigned long arg3,
                  unsigned long arg4)
{
    const char __user *model_name;
    const void __user *input;
    void __user       *output;
    size_t input_size, output_size;
    char model_buf[AI_MODEL_NAME_LEN];
    void *kbuf_in  = NULL;
    void *kbuf_out = NULL;
    long ret;
    u64 latency_ns;

    model_name  = (const char __user *)arg0;
    input       = (const void __user *)arg1;
    input_size  = (size_t)arg2;
    output      = (void __user *)arg3;
    output_size = (size_t)arg4;

    /* 参数校验 */
    if (!model_name || !input || !output)
        return -EINVAL;

    if (input_size == 0 || input_size > PAGE_SIZE * 16)
        return -EINVAL;

    if (output_size == 0 || output_size > PAGE_SIZE * 16)
        return -EINVAL;

    /* 从用户态复制模型名 */
    if (strncpy_from_user(model_buf, model_name, AI_MODEL_NAME_LEN - 1) < 0)
        return -EFAULT;
    model_buf[AI_MODEL_NAME_LEN - 1] = '\0';

    /* 分配内核缓冲区 */
    kbuf_in = kvmalloc(input_size, GFP_KERNEL);
    kbuf_out = kvmalloc(output_size, GFP_KERNEL);
    if (!kbuf_in || !kbuf_out) {
        ret = -ENOMEM;
        goto out;
    }

    /* 复制输入 */
    if (copy_from_user(kbuf_in, input, input_size)) {
        ret = -EFAULT;
        goto out;
    }

    /* 执行推理 */
    ret = ai_infer_sync(model_buf, kbuf_in, input_size,
                        kbuf_out, output_size, &latency_ns);
    if (ret)
        goto out;

    /* 复制输出 */
    if (copy_to_user(output, kbuf_out, min(output_size, input_size))) {
        ret = -EFAULT;
        goto out;
    }

    ret = min((long)output_size, (long)input_size);

out:
    kvfree(kbuf_in);
    kvfree(kbuf_out);
    return ret;
}

/* =========================================================================
 * 自我检测
 * ========================================================================= */

int ai_self_check(void)
{
    ai_model_t *model;
    void *test_weights;
    ai_result_t result;
    int ret;

    /* 分配测试权重（最小 4KB） */
    test_weights = kvmalloc(4096, GFP_KERNEL);
    if (!test_weights)
        return -ENOMEM;

    /* 注册测试模型 */
    model = ai_model_register("self_check", 1,
                               test_weights, 4096,
                               2, 16, 4);
    if (IS_ERR(model)) {
        ret = PTR_ERR(model);
        goto out;
    }

    /* 同步推理测试 */
    struct ai_task_features f = { 0 };
    f.sum_exec_runtime = 1000000000ULL;  /* 1s */
    f.nvcsw = 10;
    f.nivcsw = 2;
    f.cpu_util = 512;
    f.io_wait_ns = 50000000ULL;
    f.prio = 120;

    ret = ai_infer_sync("self_check", &f, sizeof(f),
                        &result, sizeof(result), NULL);

    ai_model_put(model);
    ai_model_unregister("self_check");

out:
    kvfree(test_weights);
    if (ret == 0)
        pr_info("%s: self-check PASSED (score=%.4f)\n",
                DRV_NAME, result.score);
    else
        pr_err("%s: self-check FAILED (err=%d)\n", DRV_NAME, ret);

    return ret;
}

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init ai_core_init(void)
{
    struct ctl_table_header *hdr;

    pr_info("============================================================\n");
    pr_info("  AI Linux Core v%s — initializing\n", DRV_VER);
    pr_info("  Build: %s %s\n", __DATE__, __TIME__);
    pr_info("============================================================\n");

    /* 创建工作队列 */
    ai_wq = alloc_workqueue("ai_infer_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    if (!ai_wq) {
        pr_err("%s: failed to alloc workqueue\n", DRV_NAME);
        return -ENOMEM;
    }

    /* 注册默认软件推理模型（占位） */
    /* 真实场景由子模块注册具体模型 */

    /* procfs */
    ai_proc_init();

    /* debugfs */
    ai_debugfs_init();

    /* sysctl */
    hdr = register_sysctl_table(ai_sysctl_root);
    if (!hdr)
        pr_warn("%s: sysctl registration failed\n", DRV_NAME);

    /* 自我检测 */
    if (ai_self_check() != 0)
        pr_warn("%s: self-check warning — continuing anyway\n", DRV_NAME);

    pr_info("%s: AI Core subsystem ready\n", DRV_NAME);
    return 0;
}

static void __exit ai_core_exit(void)
{
    ai_model_t *m, *tmp_m;
    struct ai_device *d, *tmp_d;

    pr_info("%s: shutting down AI Core\n", DRV_NAME);

    /* 等待所有异步推理完成 */
    if (ai_wq)
        destroy_workqueue(ai_wq);

    /* 卸载所有模型 */
    write_lock(&ai_lock);
    list_for_each_entry_safe(m, tmp_m, &ai_models, list) {
        list_del(&m->list);
        if (m->weights)
            ClearPageReserved(virt_to_page(m->weights));
        kfree(m);
    }
    list_for_each_entry_safe(d, tmp_d, &ai_devices, list) {
        list_del(&d->list);
        kfree(d);
    }
    write_unlock(&ai_lock);

    ai_proc_exit();
    ai_debugfs_exit();

    unregister_sysctl_table(ai_sysctl_root);

    pr_info("%s: AI Core stopped\n", DRV_NAME);
}

module_init(ai_core_init);
module_exit(ai_core_exit);

MODULE_DESCRIPTION("AI Linux Core — Kernel AI Inference Subsystem");
MODULE_VERSION(DRV_VER);
MODULE_AUTHOR("AI Linux Team");
MODULE_LICENSE("GPL v2");
MODULE_INFO(build, __DATE__ " " __TIME__);
