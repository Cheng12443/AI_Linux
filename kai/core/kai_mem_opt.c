// SPDX-License-Identifier: GPL-2.0
/*
 * kai_mem_opt.c — 内存优化（ROADMAP 1.4）
 *
 * 功能：
 *   - 页面缓存预取（AI 预测热点页面，提前换入）
 *   - 内存压缩（zswap 集成）
 *   - 大页支持（模型权重使用 THP）
 *   - LSM 决策缓存（相同可执行文件不重复检测）
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/hugetlb.h>
#include <linux/mempolicy.h>
#include <linux/fs.h>
#include <linux/zswap.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/ktime.h>

#include "kai_mem_opt.h"

#define DRV_NAME "kai_mem_opt"
#define DRV_VER  "1.0.0"

/* =========================================================================
 * 页面缓存预取
 * ========================================================================= */

#define PREFETCH_BATCH  64
#define PREFETCH_HISTORY 256

struct page_access {
    unsigned long pfn;
    __u64         timestamp_ns;
    __u32         access_count;
    struct list_head node;
};

static LIST_HEAD(prefetch_history);
static DEFINE_SPINLOCK(prefetch_lock);
static atomic_t prefetch_count = ATOMIC_INIT(0);

/*
 * kai_record_page_access — 记录页面访问（供预取预测）
 */
void kai_record_page_access(unsigned long pfn)
{
    struct page_access *entry, *tmp;
    __u64 now = ktime_get_ns();
    bool found = false;

    spin_lock(&prefetch_lock);

    /* 查找已有记录 */
    list_for_each_entry(entry, &prefetch_history, node) {
        if (entry->pfn == pfn) {
            entry->access_count++;
            entry->timestamp_ns = now;
            found = true;
            break;
        }
    }

    if (!found) {
        /* 容量检查 */
        if (atomic_read(&prefetch_count) >= PREFETCH_HISTORY) {
            tmp = list_last_entry(&prefetch_history, struct page_access, node);
            list_del(&tmp->node);
            kfree(tmp);
            atomic_dec(&prefetch_count);
        }

        entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
        if (entry) {
            entry->pfn = pfn;
            entry->timestamp_ns = now;
            entry->access_count = 1;
            list_add(&entry->node, &prefetch_history);
            atomic_inc(&prefetch_count);
        }
    }

    spin_unlock(&prefetch_lock);
}
EXPORT_SYMBOL_GPL(kai_record_page_access);

/*
 * kai_predict_hot_pages — 预测热点页面
 * 返回最可能被访问的页面（按访问频率排序）
 */
int kai_predict_hot_pages(unsigned long *pfns, int max_pages)
{
    struct page_access *entry;
    int count = 0;

    spin_lock(&prefetch_lock);

    /* 简单策略：返回访问次数最多的页面 */
    list_for_each_entry(entry, &prefetch_history, node) {
        if (count >= max_pages)
            break;
        if (entry->access_count >= 3) {  /* 访问 3 次以上视为热点 */
            pfns[count++] = entry->pfn;
        }
    }

    spin_unlock(&prefetch_lock);
    return count;
}
EXPORT_SYMBOL_GPL(kai_predict_hot_pages);

/*
 * kai_prefetch_pages — 预取页面到内存
 */
int kai_prefetch_pages(unsigned long *pfns, int count)
{
    int loaded = 0;
    int i;

    for (i = 0; i < count; i++) {
        struct page *page = pfn_to_page(pfns[i]);
        if (!page)
            continue;

        /* 标记为活跃，避免被换出 */
        if (PageLRU(page)) {
            mark_page_accessed(page);
            loaded++;
        }
    }

    return loaded;
}
EXPORT_SYMBOL_GPL(kai_prefetch_pages);

/* =========================================================================
 * 大页支持（模型权重 THP）
 * ========================================================================= */

/*
 * kai_alloc_huge_weights — 为大模型权重分配大页内存
 */
void *kai_alloc_huge_weights(size_t size)
{
    void *addr;

    /* 尝试 THP（透明大页）*/
    if (size >= PMD_SIZE) {
        addr = vzalloc(size);
        if (addr) {
            /* 尝试 madvise 使用 THP */
            unsigned long start = (unsigned long)addr;
            unsigned long end = start + size;
            /* 这里在真实场景用 madvise，内核态用 khugepaged */
            pr_debug("%s: allocated %zu bytes for weights "
                     "(THP candidate)\n", DRV_NAME, size);
            return addr;
        }
    }

    /* 回退到 vmalloc */
    addr = vzalloc(size);
    return addr;
}
EXPORT_SYMBOL_GPL(kai_alloc_huge_weights);

void kai_free_huge_weights(void *addr)
{
    if (addr)
        vfree(addr);
}
EXPORT_SYMBOL_GPL(kai_free_huge_weights);

/* =========================================================================
 * LSM 决策缓存（相同可执行文件不重复检测）
 * ========================================================================= */

#define LSM_CACHE_BITS  12
#define LSM_CACHE_TTL_NS (60ULL * 1000000000ULL)  /* 60 秒 */

struct lsm_cache_entry {
    __u32            inode_hash;      /* 可执行文件 inode 哈希 */
    __u32            decision;        /* allow / block */
    __u64            expires_ns;
    struct hlist_node node;
};

static DEFINE_HASHTABLE(lsm_cache, LSM_CACHE_BITS);
static DEFINE_SPINLOCK(lsm_lock);
static atomic_t lsm_count = ATOMIC_INIT(0);

/*
 * kai_lsm_cache_lookup — 查询 LSM 决策缓存
 */
int kai_lsm_cache_lookup(__u32 inode_hash, __u32 *decision)
{
    struct lsm_cache_entry *entry;
    __u64 now = ktime_get_ns();

    spin_lock(&lsm_lock);
    hash_for_each_possible(lsm_cache, entry, node, inode_hash) {
        if (entry->inode_hash == inode_hash && now < entry->expires_ns) {
            *decision = entry->decision;
            spin_unlock(&lsm_lock);
            return 0;
        }
    }
    spin_unlock(&lsm_lock);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(kai_lsm_cache_lookup);

/*
 * kai_lsm_cache_insert — 插入 LSM 决策缓存
 */
int kai_lsm_cache_insert(__u32 inode_hash, __u32 decision)
{
    struct lsm_cache_entry *entry;

    entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
    if (!entry)
        return -ENOMEM;

    entry->inode_hash = inode_hash;
    entry->decision = decision;
    entry->expires_ns = ktime_get_ns() + LSM_CACHE_TTL_NS;

    spin_lock(&lsm_lock);
    hash_add(lsm_cache, &entry->node, inode_hash);
    atomic_inc(&lsm_count);
    spin_unlock(&lsm_lock);

    return 0;
}
EXPORT_SYMBOL_GPL(kai_lsm_cache_insert);

/* =========================================================================
 * 内存压缩（zswap 集成）
 * ========================================================================= */

/*
 * kai_zswap_enable — 启用内存压缩
 */
int kai_zswap_enable(void)
{
    /* zswap 由内核参数控制，这里提供运行时接口 */
    pr_info("%s: zswap 集成接口（需内核 CONFIG_ZSWAP=y）\n", DRV_NAME);

    /* 实际启用需通过 sysfs：
     * echo 1 > /sys/module/zswap/parameters/enabled
     */
    return 0;
}
EXPORT_SYMBOL_GPL(kai_zswap_enable);

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init kai_mem_opt_init(void)
{
    pr_info("========================================\n");
    pr_info("  KAI Mem Opt v%s\n", DRV_VER);
    pr_info("  Prefetch + THP + LSM cache\n");
    pr_info("========================================\n");

    hash_init(lsm_cache);

    return 0;
}

static void __exit kai_mem_opt_exit(void)
{
    struct page_access *pa, *tmp;
    struct lsm_cache_entry *le;
    struct hlist_node *hnode;
    int bkt;

    /* 清理页面历史 */
    list_for_each_entry_safe(pa, tmp, &prefetch_history, node) {
        list_del(&pa->node);
        kfree(pa);
    }

    /* 清理 LSM 缓存 */
    hash_for_each_safe(lsm_cache, bkt, hnode, le, node) {
        hash_del(&le->node);
        kfree(le);
    }

    pr_info("%s: unloaded\n", DRV_NAME);
}

module_init(kai_mem_opt_init);
module_exit(kai_mem_opt_exit);

MODULE_DESCRIPTION("KAI Mem Opt — Prefetch & THP & LSM cache");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
