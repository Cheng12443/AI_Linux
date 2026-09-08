// SPDX-License-Identifier: GPL-2.0
/*
 * hello_ai.c — AI Linux 第一个内核模块
 *
 * 功能：
 *  - 模块加载/卸载时打印消息
 *  - 演示如何调用 ai_core 推理接口
 *  - 在 /proc/hello_ai 提供简单的 AI 推理入口
 *
 * 编译：
 *   cd ai_linux/kbuild
 *   make hello_ai
 *
 * 测试：
 *   insmod hello_ai.ko
 *   cat /proc/hello_ai
 *   echo 1 > /proc/hello_ai/infer
 *   rmmod hello_ai
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/cpumask.h>

#include "../ai_core/include/ai_core.h"

#define DRV_NAME  "hello_ai"
#define DRV_VER   "1.0.0"

static struct proc_dir_entry *hello_proc_dir;

/* =========================================================================
 * 推理测试函数
 * ========================================================================= */

static int test_sched_inference(void)
{
    struct ai_task_features f = { 0 };
    ai_result_t result = { 0 };
    int ret;
    u64 latency_ns = 0;

    /* 构造一个模拟任务特征 */
    f.sum_exec_runtime = 5000000000ULL;  /* 5 秒 */
    f.nvcsw            = 25;
    f.nivcsw           = 5;
    f.cpu_util         = 768;            /* ~75% CPU */
    f.io_wait_ns       = 200000000ULL;   /* 200ms IO 等待 */
    f.cache_miss_rate  = 150;            /* 15% 缓存未命中率 */
    f.mem_usage_kb     = 256000;         /* 256MB */
    f.prio             = 120;            /* 普通优先级 */
    f.oom_score_adj    = 0;

    strscpy(f.comm, "test_task", sizeof(f.comm));

    /* 调用 AI 推理接口 */
    ret = ai_infer_sync("sched_decision",
                        &f, sizeof(f),
                        &result, sizeof(result),
                        &latency_ns);

    if (ret) {
        pr_warn("%s: ai_infer_sync failed: %d\n", DRV_NAME, ret);
        return ret;
    }

    pr_info("%s: 推理结果 — score=%.4f latency=%llu ns\n",
            DRV_NAME, result.score, latency_ns);

    /* 根据结果做调度决策演示 */
    if (result.score * 100 > 70) {
        pr_info("%s: AI 建议升权（AI_PROMOTE）\n", DRV_NAME);
    } else if (result.score * 100 < 30) {
        pr_info("%s: AI 建议降权（AI_DEMOTE）\n", DRV_NAME);
    } else {
        pr_info("%s: AI 建议保持（AI_KEEP）\n", DRV_NAME);
    }

    return 0;
}

/* =========================================================================
 * procfs 接口
 * ========================================================================= */

static int hello_show(struct seq_file *m, void *v)
{
    struct ai_stats stats;
    struct ai_task_features f;
    struct task_struct *task;

    seq_printf(m, "========================================\n");
    seq_printf(m, "  Hello AI — AI Linux Kernel Module\n");
    seq_printf(m, "  v%s | Built: %s %s\n", DRV_VER, __DATE__, __TIME__);
    seq_printf(m, "========================================\n\n");

    /* 系统统计 */
    ai_stats_read(&stats);
    seq_printf(m, "AI Core 统计:\n");
    seq_printf(m, "  推理总数:     %llu\n", stats.total_inferences);
    seq_printf(m, "  同步:         %llu\n", stats.sync_inferences);
    seq_printf(m, "  异步:         %llu\n", stats.async_inferences);
    seq_printf(m, "  错误:         %llu\n", stats.errors);
    seq_printf(m, "  平均延迟:     %llu ns\n",
               stats.sync_inferences ?
               stats.sync_total_ns / stats.sync_inferences : 0);
    seq_printf(m, "\n");

    /* 当前进程示例 */
    task = current;
    seq_printf(m, "当前进程:\n");
    seq_printf(m, "  comm:         %s\n", task->comm);
    seq_printf(m, "  PID:          %d\n", task->pid);
    seq_printf(m, "  CPU:          %d\n", task_cpu(task));
    seq_printf(m, "  优先级:       %d\n", task->prio);
    seq_printf(m, "\n");

    /* CPU 信息 */
    seq_printf(m, "系统 CPU:\n");
    seq_printf(m, "  在线 CPU:     %d\n", num_online_cpus());
    seq_printf(m, "  全部 CPU:     %d\n", num_possible_cpus());
    seq_printf(m, "\n");

    seq_printf(m, "提示：echo 1 > /proc/hello_ai/infer 触发推理测试\n");
    return 0;
}

static int hello_open(struct inode *inode, struct file *file)
{
    return single_open(file, hello_show, NULL);
}

static ssize_t hello_infer_write(struct file *file, const char __user *buf,
                                  size_t count, loff_t *ppos)
{
    char kbuf[16];

    if (count >= sizeof(kbuf))
        return -EINVAL;

    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    kbuf[count] = '\0';

    if (kbuf[0] == '1' || kbuf[0] == 'y') {
        pr_info("%s: 触发调度推理测试...\n", DRV_NAME);
        test_sched_inference();
    } else {
        pr_info("%s: 未知命令: %s\n", DRV_NAME, kbuf);
    }

    return count;
}

static const struct proc_ops hello_proc_fops = {
    .proc_open    = hello_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
    .proc_write   = hello_infer_write,
};

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init hello_ai_init(void)
{
    pr_info("============================================================\n");
    pr_info("  Hello AI — AI Linux Kernel Module Loaded\n");
    pr_info("  演示内核级 AI 推理接口调用\n");
    pr_info("  AI Core 版本: %s\n", ai_version_string());
    pr_info("  编译时间: %s %s\n", __DATE__, __TIME__);
    pr_info("============================================================\n");

    /* 创建 proc 条目 */
    hello_proc_dir = proc_mkdir("hello_ai", NULL);
    if (!hello_proc_dir) {
        pr_err("%s: failed to create /proc/hello_ai\n", DRV_NAME);
        return -ENOMEM;
    }

    proc_create("stats", 0444, hello_proc_dir, &hello_proc_fops);
    proc_create("infer", 0222, hello_proc_dir, &hello_proc_fops);

    pr_info("%s: 模块初始化完成，查看 /proc/hello_ai/stats\n", DRV_NAME);
    pr_info("%s: 触发推理: echo 1 > /proc/hello_ai/infer\n", DRV_NAME);

    return 0;
}

static void __exit hello_ai_exit(void)
{
    pr_info("%s: 卸载 AI Hello 模块\n", DRV_NAME);

    if (hello_proc_dir) {
        remove_proc_entry("stats", hello_proc_dir);
        remove_proc_entry("infer", hello_proc_dir);
        remove_proc_entry("hello_ai", NULL);
    }

    pr_info("%s: 再见！\n", DRV_NAME);
}

module_init(hello_ai_init);
module_exit(hello_ai_exit);

MODULE_DESCRIPTION("Hello AI — AI Linux 第一个内核模块");
MODULE_VERSION(DRV_VER);
MODULE_AUTHOR("AI Linux Team");
MODULE_LICENSE("GPL v2");
