// SPDX-License-Identifier: GPL-2.0
/*
 * sched_plugin.c — 调度插件（内置）
 *
 * 通过插件管理器注册，提供 AI 调度决策能力。
 *
 * 注册方式：
 *   module_ai_plugin(sched_plugin_ops)
 *
 * 或手动：
 *   ai_plugin_register(&sched_plugin_ops);
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/sched/signal.h>
#include <linux/sched/stat.h>

#include "../core/ai_plugin.h"
#include "../../ai_core/include/ai_core.h"

#define DRV_NAME  "sched_plugin"
#define DRV_VER   "1.0.0"

/* =========================================================================
 * 配置参数
 * ========================================================================= */

static int promote_threshold   = 70;
static int demote_threshold    = 30;
static int migrate_threshold   = 60;
static int batch_threshold     = 40;
static int confidence_threshold = 75;

module_param(promote_threshold,   int, 0644);
module_param(demote_threshold,    int, 0644);
module_param(migrate_threshold,   int, 0644);
module_param(batch_threshold,     int, 0644);
module_param(confidence_threshold, int, 0644);

/* =========================================================================
 * 调度插件实现
 * ========================================================================= */

/*
 * sched_plugin_infer — 调度推理
 *
 * 输入：ai_infer_ctx 包含任务特征
 * 输出：调度决策（promote/demote/migrate/batch/keep）
 */
static int sched_plugin_infer(struct ai_infer_ctx *ctx)
{
    struct ai_task_features *f = (struct ai_task_features *)ctx->input;
    __u8 *out = (__u8 *)ctx->output;
    int score = 0;

    if (!f || !out || ctx->output_size < sizeof(__u8))
        return -EINVAL;

    /* 评分计算（启发式）*/
    /* 1. CPU 利用率：0-1024 */
    if (f->cpu_util > 512) score += 30;
    else if (f->cpu_util > 256) score += 15;

    /* 2. 非自愿切换：IO 密集的标志 */
    if (f->nivcsw > 20) score += 25;
    else if (f->nivcsw > 10) score += 10;

    /* 3. 自愿切换：高 IO 密集 */
    if (f->nvcsw > 100) score += 20;

    /* 4. IO 等待时间 */
    if (f->io_wait_ns > 100000000ULL) score += 15; /* >100ms */

    /* 5. 缓存未命中 */
    if (f->cache_miss_rate > 500) score -= 10;

    /* 6. 优先级：低优先级任务考虑降权 */
    if (f->prio > 140) score -= 5;

    /* 归一化到 0-100 */
    if (score < 0) score = 0;
    if (score > 100) score = 100;

    /* 决策映射 */
    if (score > promote_threshold)
        *out = AI_PROMOTE;
    else if (score < demote_threshold)
        *out = AI_DEMOTE;
    else if (score > migrate_threshold)
        *out = AI_MIGRATE;
    else if (score > batch_threshold && f->nvcsw > 500)
        *out = AI_BATCH;
    else
        *out = AI_KEEP;

    /* 写入元数据 */
    if (ctx->output_size >= sizeof(ai_result_t)) {
        ai_result_t *r = (ai_result_t *)ctx->output;
        r->score = (float)score / 100.0f;
        r->top_class.confidence = (int)score;
        r->top_class.label = *out;
    }

    return 0;
}

/* 预处理：将 task_struct 数据转换为 ai_task_features */
static int sched_preprocess(void *input, size_t in_size,
                            void **out, size_t *out_size)
{
    /* 这里可以提取 /proc 或内核调度器数据 */
    *out = input;
    *out_size = in_size;
    return 0;
}

/* 后处理：将推理结果转换为 sched_ext 可用的格式 */
static int sched_postprocess(void *input, size_t in_size,
                              void *output, size_t out_size,
                              void **result, size_t *result_size)
{
    (void)input; (void)in_size;
    *result = output;
    *result_size = out_size;
    return 0;
}

/* 配置参数处理 */
static int sched_set_param(const char *key, const void *val, size_t val_size)
{
    if (val_size != sizeof(int)) return -EINVAL;
    int v = *(const int *)val;

    if (strcmp(key, "promote_threshold") == 0) promote_threshold = v;
    else if (strcmp(key, "demote_threshold") == 0) demote_threshold = v;
    else if (strcmp(key, "migrate_threshold") == 0) migrate_threshold = v;
    else if (strcmp(key, "batch_threshold") == 0) batch_threshold = v;
    else if (strcmp(key, "confidence_threshold") == 0) confidence_threshold = v;
    else return -ENOENT;

    return 0;
}

static int sched_get_param(const char *key, void *val, size_t *val_size)
{
    if (*val_size != sizeof(int)) return -EINVAL;
    int v = 0;

    if (strcmp(key, "promote_threshold") == 0) v = promote_threshold;
    else if (strcmp(key, "demote_threshold") == 0) v = demote_threshold;
    else if (strcmp(key, "migrate_threshold") == 0) v = migrate_threshold;
    else if (strcmp(key, "batch_threshold") == 0) v = batch_threshold;
    else if (strcmp(key, "confidence_threshold") == 0) v = confidence_threshold;
    else return -ENOENT;

    *(int *)val = v;
    return 0;
}

/* =========================================================================
 * 插件操作接口
 * ========================================================================= */

static struct ai_plugin_ops sched_plugin_ops = {
    .name           = "sched_ai",
    .version        = "1.0.0",
    .description    = "AI 调度决策插件（promote/demote/migrate）",
    .type           = PLUGIN_TYPE_SCHED,
    .api_version   = AI_PLUGIN_VERSION,
    .init           = NULL,  /* 无额外初始化 */
    .exit           = NULL,
    .infer          = sched_plugin_infer,
    .preprocess     = sched_preprocess,
    .postprocess    = sched_postprocess,
    .set_param      = sched_set_param,
    .get_param      = sched_get_param,
};

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init sched_plugin_init(void)
{
    int ret = ai_plugin_register(&sched_plugin_ops);
    if (ret)
        return ret;

    pr_info("%s: loaded\n", DRV_NAME);
    return 0;
}

static void __exit sched_plugin_exit(void)
{
    ai_plugin_unregister("sched_ai");
    pr_info("%s: unloaded\n", DRV_NAME);
}

module_init(sched_plugin_init);
module_exit(sched_plugin_exit);

MODULE_DESCRIPTION("AI Linux Scheduling Plugin");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
