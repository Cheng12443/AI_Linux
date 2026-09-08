// SPDX-License-Identifier: GPL-2.0
/*
 * ai_memory.c — AI 驱动的内存管理增强
 *
 * 插桩到 mm/vmscan.c 的 kswapd 路径中：
 *   - 基于历史页面访问模式预测哪些页面即将被访问
 *   - 调整 kswapd 的扫描优先级和换出策略
 *
 * 核心函数：ai_swap_predict()
 *   输入：最近 N 个进程的页面引用历史
 *   输出：按热度排序的页面列表（最可能被访问的优先换入）
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/vmstat.h>
#include <linux/workqueue.h>
#include "../include/ai_core.h"

#define DRV_NAME  "ai_memory"
#define DRV_VER   "1.0.0"

#define AI_MEM_HISTORY_LEN  64
#define AI_PAGE_POOL_SIZE   256

/* 页面访问历史条目 */
struct page_access_record {
    unsigned long pfn;
    __u32 pid;
    __u32 access_flags;   /* bit0=read, bit1=write, bit2=exec */
    __u64 timestamp_ns;
    __u32 cpu_id;
};

/* 预测结果 */
struct page_prediction {
    unsigned long pfn;
    float         score;      /* 0.0~1.0，热度评分 */
    __u32         pid;
    int           action;      /* 0=swap_in, 1=keep, 2=swap_out */
};

static struct {
    struct page_access_record history[AI_MEM_HISTORY_LEN];
    int head;
    spinlock_t lock;
    int enabled;
} ai_page_tracker = {
    .lock = __SPIN_LOCK_UNLOCKED(ai_page_tracker.lock),
    .enabled = 1,
};

static struct page_prediction ai_predictions[AI_PAGE_POOL_SIZE];
static int num_predictions;

/* =========================================================================
 * 页面访问记录（供 kswapd 等调用方使用）
 * ========================================================================= */

/*
 * ai_record_page_access — 记录一次页面访问
 *
 * 在 __lru_cache_add()、handle_pte_fault() 等路径调用
 * 由具体子系统（如文件系统和调度器）主动上报
 */
void ai_record_page_access(unsigned long pfn, __u32 pid,
                            __u32 flags, __u32 cpu)
{
    unsigned long irqflags;
    struct page_access_record *rec;

    if (!ai_page_tracker.enabled)
        return;

    spin_lock_irqsave(&ai_page_tracker.lock, irqflags);

    rec = &ai_page_tracker.history[ai_page_tracker.head];
    rec->pfn         = pfn;
    rec->pid         = pid;
    rec->access_flags = flags;
    rec->timestamp_ns = __ktime_get_real_ns();
    rec->cpu_id      = cpu;

    ai_page_tracker.head = (ai_page_tracker.head + 1) % AI_MEM_HISTORY_LEN;

    spin_unlock_irqrestore(&ai_page_tracker.lock, irqflags);
}
EXPORT_SYMBOL_GPL(ai_record_page_access);

/* =========================================================================
 * AI 预测引擎（最简演示）
 * ========================================================================= */

/*
 * ai_swap_predict — 预测即将被访问的页面
 *
 * 真实场景：用 LSTM/Transformer 模型分析时间序列访问模式。
 * 这里用最简单的 MRU（最近最少使用）启发式：
 *   1. 高频率访问的 PFN 评分高
 *   2. 最近访问的 PFN 评分高
 *   3. 有写标志的页面降权（写时复制等）
 */
int ai_swap_predict(struct page_prediction *out, int max_out,
                    int numa_node)
{
    struct page_access_record rec;
    unsigned long irqflags;
    int i, j, n = 0;

    if (!ai_page_tracker.enabled || !out || max_out == 0)
        return 0;

    /* 清零预测池 */
    memset(out, 0, sizeof(*out) * max_out);
    num_predictions = 0;

    /* 扫描历史，计算 PFN 频率和最近度 */
    spin_lock_irqsave(&ai_page_tracker.lock, irqflags);

    for (i = 0; i < AI_MEM_HISTORY_LEN; i++) {
        int idx = (ai_page_tracker.head - 1 - i + AI_MEM_HISTORY_LEN) % AI_MEM_HISTORY_LEN;
        rec = ai_page_tracker.history[idx];

        if (rec.pfn == 0)
            continue;

        /* 在 out 中查找是否已记录该 PFN */
        int found = -1;
        for (j = 0; j < n; j++) {
            if (out[j].pfn == rec.pfn) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            /* 增加频率分，衰减近期度 */
            out[found].score += 0.1f + 0.05f * (AI_MEM_HISTORY_LEN - i);
            if (out[found].score > 1.0f)
                out[found].score = 1.0f;
        } else if (n < max_out) {
            /* 新记录 */
            out[n].pfn   = rec.pfn;
            out[n].pid   = rec.pid;
            out[n].score = 0.1f + 0.05f * (AI_MEM_HISTORY_LEN - i);
            if (out[n].score > 1.0f)
                out[n].score = 1.0f;

            /* 写访问降权（容易被换走）*/
            if (rec.access_flags & 2)
                out[n].score *= 0.7f;

            n++;
        }
    }

    spin_unlock_irqrestore(&ai_page_tracker.lock, irqflags);

    /* 按分数降序排序（最热页面排前面）*/
    for (i = 0; i < n - 1; i++) {
        for (j = i + 1; j < n; j++) {
            if (out[j].score > out[i].score) {
                struct page_prediction tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }

    /* 分配动作：前 1/3 标记 swap_in，后 1/3 标记 swap_out */
    for (i = 0; i < n; i++) {
        if (i < n / 3)
            out[i].action = 0; /* swap_in */
        else if (i > 2 * n / 3)
            out[i].action = 2; /* swap_out */
        else
            out[i].action = 1; /* keep */
    }

    num_predictions = n;
    return n;
}
EXPORT_SYMBOL_GPL(ai_swap_predict);

/* =========================================================================
 * 统计接口
 * ========================================================================= */

int ai_get_swap_predictions(struct page_prediction *buf, int max)
{
    int n = min(num_predictions, max);
    memcpy(buf, ai_predictions, n * sizeof(struct page_prediction));
    return n;
}
EXPORT_SYMBOL_GPL(ai_get_swap_predictions);

static int ai_mem_stats_show(struct seq_file *m, void *v)
{
    int n = ai_get_swap_predictions(ai_predictions,
                                    AI_PAGE_POOL_SIZE);

    seq_printf(m, "AI Memory Manager v%s\n", DRV_VER);
    seq_printf(m, "enabled: %d\n", ai_page_tracker.enabled);
    seq_printf(m, "history entries: %d / %d\n",
               AI_MEM_HISTORY_LEN, AI_MEM_HISTORY_LEN);
    seq_printf(m, "predictions: %d\n\n", n);

    seq_printf(m, "%-12s %-10s %-10s %-8s\n",
               "PFN", "PID", "Score", "Action");
    seq_printf(m, "%-12s %-10s %-10s %-8s\n",
               "---", "---", "-----", "------");

    for (int i = 0; i < n; i++) {
        const char *action_str[] = { "swap_in", "keep", "swap_out" };
        seq_printf(m, "0x%-10lx %-10d %-10.4f %-8s\n",
                    ai_predictions[i].pfn,
                    ai_predictions[i].pid,
                    ai_predictions[i].score,
                    action_str[ai_predictions[i].action]);
    }

    /* 全局内存状态 */
    seq_printf(m, "\n系统内存:\n");
    seq_printf(m, "  nr_free_pages:    %lu\n", nr_free_pages());
    seq_printf(m, "  nr_swapcache:     %lu\n", nr_swapcache());
    seq_printf(m, "  totalram:         %lu pages\n",
               totalram_pages());

    return 0;
}

static int ai_mem_open(struct inode *inode, struct file *file)
{
    return single_open(file, ai_mem_stats_show, NULL);
}

static ssize_t ai_mem_write(struct file *file, const char __user *buf,
                            size_t count, loff_t *ppos)
{
    char kbuf[16];
    if (count >= sizeof(kbuf)) return -EINVAL;
    if (copy_from_user(kbuf, buf, count)) return -EFAULT;
    kbuf[count] = '\0';

    if (kbuf[0] == '0') {
        ai_page_tracker.enabled = 0;
        pr_info("%s: AI memory predictor disabled\n", DRV_NAME);
    } else {
        ai_page_tracker.enabled = 1;
        pr_info("%s: AI memory predictor enabled\n", DRV_NAME);
    }
    return count;
}

static const struct proc_ops ai_mem_proc_fops = {
    .proc_open    = ai_mem_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
    .proc_write   = ai_mem_write,
};

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init ai_memory_init(void)
{
    pr_info("%s: AI Memory Manager v%s loaded\n", DRV_NAME, DRV_VER);

    proc_create("ai_memory", 0644, NULL, &ai_mem_proc_fops);

    pr_info("%s: /proc/ai_memory — 查看页面热度预测\n", DRV_NAME);
    return 0;
}

static void __exit ai_memory_exit(void)
{
    remove_proc_entry("ai_memory", NULL);
    pr_info("%s: unloaded\n", DRV_NAME);
}

module_init(ai_memory_init);
module_exit(ai_memory_exit);

MODULE_DESCRIPTION("AI Memory — AI-driven memory management");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
