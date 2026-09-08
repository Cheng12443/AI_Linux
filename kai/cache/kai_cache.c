// SPDX-License-Identifier: GPL-2.0
/*
 * kai_cache.c — 多级缓存实现
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/rcupdate.h>

#include "kai_cache.h"

#define DRV_NAME "kai_cache"
#define DRV_VER  "1.0.0"

/* 全局缓存池 */
static struct kmem_cache *entry_slab;
static struct kmem_cache *data_slab;

/* =========================================================================
 * 创建 / 销毁
 * ========================================================================= */

struct kai_cache *kai_cache_create(const char *name,
                                    __u32 max_entries,
                                    __u32 policy,
                                    __u32 entry_size,
                                    __u32 default_ttl_ms)
{
    struct kai_cache *cache;

    cache = kzalloc(sizeof(*cache), GFP_KERNEL);
    if (!cache)
        return NULL;

    strscpy(cache->name, name, sizeof(cache->name));
    cache->max_entries = max_entries ?: KAI_CACHE_MAX_ENTRIES;
    cache->policy = policy;
    cache->entry_size = entry_size;
    cache->default_ttl_ms = default_ttl_ms ?: KAI_CACHE_DEFAULT_TTL_MS;
    cache->hash_bits = ilog2(max_entries) + 1;

    spin_lock_init(&cache->lock);
    INIT_LIST_HEAD(&cache->lru_list);

    /* 分配哈希表 */
    cache->hash_table = kcalloc(1 << cache->hash_bits,
                                 sizeof(struct hlist_head), GFP_KERNEL);
    if (!cache->hash_table) {
        kfree(cache);
        return NULL;
    }
    hash_init(cache->hash_table, cache->hash_bits);

    /* 创建 slab 缓存（如果 entry_size 固定）*/
    if (entry_size > 0) {
        char slab_name[64];
        snprintf(slab_name, sizeof(slab_name), "kai_%s_entry", name);
        entry_slab = kmem_cache_create(slab_name,
                                        sizeof(struct kai_cache_entry),
                                        0, SLAB_HWCACHE_ALIGN, NULL);
        if (!entry_slab)
            pr_warn("%s: entry slab creation failed\n", DRV_NAME);

        snprintf(slab_name, sizeof(slab_name), "kai_%s_data", name);
        data_slab = kmem_cache_create(slab_name, entry_size,
                                       0, SLAB_HWCACHE_ALIGN, NULL);
        if (!data_slab)
            pr_warn("%s: data slab creation failed\n", DRV_NAME);
    }

    pr_debug("%s: created cache '%s' max=%u policy=%u\n",
             DRV_NAME, name, max_entries, policy);

    return cache;
}
EXPORT_SYMBOL_GPL(kai_cache_create);

void kai_cache_destroy(struct kai_cache *cache)
{
    struct kai_cache_entry *entry, *tmp;
    unsigned long flags;

    if (!cache)
        return;

    spin_lock_irqsave(&cache->lock, flags);

    /* 清理所有条目 */
    list_for_each_entry_safe(entry, tmp, &cache->lru_list, lru_node) {
        hash_del(&entry->hash_node);
        list_del(&entry->lru_node);
        if (entry->data) {
            if (cache->entry_size > 0 && data_slab)
                kmem_cache_free(data_slab, entry->data);
            else
                kfree(entry->data);
        }
        if (cache->entry_size > 0 && entry_slab)
            kmem_cache_free(entry_slab, entry);
        else
            kfree(entry);
    }

    spin_unlock_irqrestore(&cache->lock, flags);

    if (cache->hash_table)
        kfree(cache->hash_table);

    if (cache->entry_size > 0) {
        if (entry_slab)
            kmem_cache_destroy(entry_slab);
        if (data_slab)
            kmem_cache_destroy(data_slab);
    }

    kfree(cache);
    pr_debug("%s: destroyed cache '%s'\n", DRV_NAME, cache->name);
}
EXPORT_SYMBOL_GPL(kai_cache_destroy);

/* =========================================================================
 * 查找
 * ========================================================================= */

static struct kai_cache_entry *
find_entry(struct kai_cache *cache, const char *key)
{
    struct kai_cache_entry *entry;
    __u32 hash = kai_cache_key_hash(key);
    struct hlist_head *head = &cache->hash_table[hash & ((1 << cache->hash_bits) - 1)];

    hlist_for_each_entry(entry, head, hash_node) {
        if (strcmp(entry->key, key) == 0)
            return entry;
    }
    return NULL;
}

/* =========================================================================
 * 存取
 * ========================================================================= */

int kai_cache_put(struct kai_cache *cache,
                  const char *key,
                  const void *data, size_t data_size)
{
    return kai_cache_put_ex(cache, key, data, data_size,
                             cache->default_ttl_ms);
}

int kai_cache_put_ex(struct kai_cache *cache,
                     const char *key,
                     const void *data, size_t data_size,
                     __u32 ttl_ms)
{
    struct kai_cache_entry *entry;
    unsigned long flags;
    void *data_copy = NULL;

    if (!cache || !key || !data || data_size == 0)
        return -EINVAL;

    /* 分配数据副本 */
    if (cache->entry_size > 0 && data_slab) {
        data_copy = kmem_cache_alloc(data_slab, GFP_ATOMIC);
    } else {
        data_copy = kmalloc(data_size, GFP_ATOMIC);
    }
    if (!data_copy)
        return -ENOMEM;
    memcpy(data_copy, data, data_size);

    spin_lock_irqsave(&cache->lock, flags);

    /* 检查是否已存在 */
    entry = find_entry(cache, key);
    if (entry) {
        /* 更新现有条目 */
        if (entry->data) {
            if (cache->entry_size > 0 && data_slab)
                kmem_cache_free(data_slab, entry->data);
            else
                kfree(entry->data);
        }
        entry->data = data_copy;
        entry->data_size = data_size;
        entry->last_access_ns = ktime_get_ns();
        entry->ttl_ms = ttl_ms;
        entry->flags |= KAI_CACHE_VALID;

        /* 移动到 LRU 头部 */
        list_move(&entry->lru_node, &cache->lru_list);
        cache->stats.inserts++;
        spin_unlock_irqrestore(&cache->lock, flags);
        return 0;
    }

    /* 检查容量 */
    if (cache->stats.inserts >= cache->max_entries) {
        /* 驱逐最老的条目 */
        struct kai_cache_entry *victim = list_last_entry(&cache->lru_list,
                                                          struct kai_cache_entry,
                                                          lru_node);
        if (victim) {
            if (cache->on_evict)
                cache->on_evict(victim);

            hash_del(&victim->hash_node);
            list_del(&victim->lru_node);
            if (victim->data) {
                if (cache->entry_size > 0 && data_slab)
                    kmem_cache_free(data_slab, victim->data);
                else
                    kfree(victim->data);
            }
            if (cache->entry_size > 0 && entry_slab)
                kmem_cache_free(entry_slab, victim);
            else
                kfree(victim);

            cache->stats.evictions++;
        }
    }

    /* 创建新条目 */
    if (cache->entry_size > 0 && entry_slab) {
        entry = kmem_cache_alloc(entry_slab, GFP_ATOMIC);
    } else {
        entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
    }

    if (!entry) {
        if (cache->entry_size > 0 && data_slab)
            kmem_cache_free(data_slab, data_copy);
        else
            kfree(data_copy);
        spin_unlock_irqrestore(&cache->lock, flags);
        return -ENOMEM;
    }

    strscpy(entry->key, key, sizeof(entry->key));
    entry->data = data_copy;
    entry->data_size = data_size;
    entry->created_ns = ktime_get_ns();
    entry->last_access_ns = entry->created_ns;
    entry->ttl_ms = ttl_ms;
    entry->flags = KAI_CACHE_VALID;
    entry->hit_count = 0;

    /* 加入哈希表和 LRU 链表 */
    __u32 hash = kai_cache_key_hash(key);
    hash_add(cache->hash_table, &entry->hash_node, hash & ((1 << cache->hash_bits) - 1));
    list_add(&entry->lru_node, &cache->lru_list);

    cache->stats.inserts++;
    cache->stats.total_size += data_size;

    spin_unlock_irqrestore(&cache->lock, flags);
    return 0;
}
EXPORT_SYMBOL_GPL(kai_cache_put_ex);

int kai_cache_get(struct kai_cache *cache,
                  const char *key,
                  void *out, size_t *out_size)
{
    struct kai_cache_entry *entry;
    unsigned long flags;
    int ret = -ENOENT;

    if (!cache || !key)
        return -EINVAL;

    spin_lock_irqsave(&cache->lock, flags);

    entry = find_entry(cache, key);
    if (!entry) {
        cache->stats.misses++;
        spin_unlock_irqrestore(&cache->lock, flags);
        return -ENOENT;
    }

    /* 检查过期 */
    if (kai_cache_entry_expired(entry)) {
        entry->flags &= ~KAI_CACHE_VALID;
        if (cache->on_expire)
            cache->on_expire(entry);
        cache->stats.expirations++;
        spin_unlock_irqrestore(&cache->lock, flags);
        return -ENOENT;
    }

    /* 复制数据 */
    if (out && out_size) {
        size_t copy_size = *out_size;
        if (copy_size > entry->data_size)
            copy_size = entry->data_size;
        memcpy(out, entry->data, copy_size);
        *out_size = entry->data_size;
        ret = 0;
    } else {
        /* 只检查存在性 */
        ret = 0;
    }

    /* 更新统计 */
    entry->hit_count++;
    entry->last_access_ns = ktime_get_ns();
    cache->stats.hits++;

    /* 移到 LRU 头部 */
    list_move(&entry->lru_node, &cache->lru_list);

    spin_unlock_irqrestore(&cache->lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(kai_cache_get);

int kai_cache_get_copy(struct kai_cache *cache,
                        const char *key,
                        void **out, size_t *out_size)
{
    struct kai_cache_entry *entry;
    unsigned long flags;
    void *data_copy;

    if (!cache || !key || !out)
        return -EINVAL;

    spin_lock_irqsave(&cache->lock, flags);

    entry = find_entry(cache, key);
    if (!entry || kai_cache_entry_expired(entry)) {
        cache->stats.misses++;
        spin_unlock_irqrestore(&cache->lock, flags);
        return -ENOENT;
    }

    /* 分配副本 */
    data_copy = kmalloc(entry->data_size, GFP_ATOMIC);
    if (!data_copy) {
        spin_unlock_irqrestore(&cache->lock, flags);
        return -ENOMEM;
    }
    memcpy(data_copy, entry->data, entry->data_size);

    *out = data_copy;
    if (out_size)
        *out_size = entry->data_size;

    entry->hit_count++;
    entry->last_access_ns = ktime_get_ns();
    cache->stats.hits++;

    list_move(&entry->lru_node, &cache->lru_list);

    spin_unlock_irqrestore(&cache->lock, flags);
    return 0;
}
EXPORT_SYMBOL_GPL(kai_cache_get_copy);

int kai_cache_has(struct kai_cache *cache, const char *key)
{
    return kai_cache_get(cache, key, NULL, NULL);
}

int kai_cache_remove(struct kai_cache *cache, const char *key)
{
    struct kai_cache_entry *entry;
    unsigned long flags;

    if (!cache || !key)
        return -EINVAL;

    spin_lock_irqsave(&cache->lock, flags);

    entry = find_entry(cache, key);
    if (!entry) {
        spin_unlock_irqrestore(&cache->lock, flags);
        return -ENOENT;
    }

    hash_del(&entry->hash_node);
    list_del(&entry->lru_node);

    if (entry->data) {
        if (cache->entry_size > 0 && data_slab)
            kmem_cache_free(data_slab, entry->data);
        else
            kfree(entry->data);
    }

    if (cache->entry_size > 0 && entry_slab)
        kmem_cache_free(entry_slab, entry);
    else
        kfree(entry);

    cache->stats.inserts--;

    spin_unlock_irqrestore(&cache->lock, flags);
    return 0;
}
EXPORT_SYMBOL_GPL(kai_cache_remove);

/* =========================================================================
 * 批量操作
 * ========================================================================= */

int kai_cache_flush(struct kai_cache *cache)
{
    struct kai_cache_entry *entry, *tmp;
    unsigned long flags;
    int count = 0;

    if (!cache)
        return -EINVAL;

    spin_lock_irqsave(&cache->lock, flags);

    list_for_each_entry_safe(entry, tmp, &cache->lru_list, lru_node) {
        hash_del(&entry->hash_node);
        list_del(&entry->lru_node);

        if (entry->data) {
            if (cache->entry_size > 0 && data_slab)
                kmem_cache_free(data_slab, entry->data);
            else
                kfree(entry->data);
        }

        if (cache->entry_size > 0 && entry_slab)
            kmem_cache_free(entry_slab, entry);
        else
            kfree(entry);

        count++;
    }

    cache->stats.inserts = 0;
    cache->stats.total_size = 0;

    spin_unlock_irqrestore(&cache->lock, flags);
    return count;
}
EXPORT_SYMBOL_GPL(kai_cache_flush);

int kai_cache_flush_expired(struct kai_cache *cache)
{
    struct kai_cache_entry *entry, *tmp;
    unsigned long flags;
    int count = 0;

    if (!cache)
        return -EINVAL;

    spin_lock_irqsave(&cache->lock, flags);

    list_for_each_entry_safe(entry, tmp, &cache->lru_list, lru_node) {
        if (!kai_cache_entry_expired(entry))
            continue;

        if (cache->on_expire)
            cache->on_expire(entry);

        hash_del(&entry->hash_node);
        list_del(&entry->lru_node);

        if (entry->data) {
            if (cache->entry_size > 0 && data_slab)
                kmem_cache_free(data_slab, entry->data);
            else
                kfree(entry->data);
        }

        if (cache->entry_size > 0 && entry_slab)
            kmem_cache_free(entry_slab, entry);
        else
            kfree(entry);

        count++;
        cache->stats.expirations++;
    }

    spin_unlock_irqrestore(&cache->lock, flags);
    return count;
}
EXPORT_SYMBOL_GPL(kai_cache_flush_expired);

/* =========================================================================
 * 统计
 * ========================================================================= */

void kai_cache_stats(struct kai_cache *cache, struct kai_cache_stats *out)
{
    unsigned long flags;

    if (!cache || !out)
        return;

    spin_lock_irqsave(&cache->lock, flags);
    memcpy(out, &cache->stats, sizeof(*out));
    spin_unlock_irqrestore(&cache->lock, flags);
}
EXPORT_SYMBOL_GPL(kai_cache_stats);

void kai_cache_reset_stats(struct kai_cache *cache)
{
    unsigned long flags;

    if (!cache)
        return;

    spin_lock_irqsave(&cache->lock, flags);
    memset(&cache->stats, 0, sizeof(cache->stats));
    spin_unlock_irqrestore(&cache->lock, flags);
}
EXPORT_SYMBOL_GPL(kai_cache_reset_stats);

/* =========================================================================
 * 迭代
 * ========================================================================= */

int kai_cache_foreach(struct kai_cache *cache, kai_cache_iter_fn fn, void *priv)
{
    struct kai_cache_entry *entry;
    unsigned long flags;
    int count = 0;

    if (!cache || !fn)
        return -EINVAL;

    spin_lock_irqsave(&cache->lock, flags);

    list_for_each_entry(entry, &cache->lru_list, lru_node) {
        fn(entry, priv);
        count++;
    }

    spin_unlock_irqrestore(&cache->lock, flags);
    return count;
}
EXPORT_SYMBOL_GPL(kai_cache_foreach);

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init kai_cache_init(void)
{
    pr_info("========================================\n");
    pr_info("  KAI Cache v%s — Multi-level cache\n", DRV_VER);
    pr_info("========================================\n");

    /* 创建全局 slab 缓存 */
    entry_slab = kmem_cache_create("kai_entry_slab",
                                    sizeof(struct kai_cache_entry),
                                    0, SLAB_HWCACHE_ALIGN, NULL);
    if (!entry_slab)
        pr_warn("%s: entry slab creation failed\n", DRV_NAME);

    return 0;
}

static void __exit kai_cache_exit(void)
{
    if (entry_slab)
        kmem_cache_destroy(entry_slab);
    if (data_slab)
        kmem_cache_destroy(data_slab);
    pr_info("%s: unloaded\n", DRV_NAME);
}

module_init(kai_cache_init);
module_exit(kai_cache_exit);

MODULE_DESCRIPTION("KAI Cache — Multi-level cache system");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
