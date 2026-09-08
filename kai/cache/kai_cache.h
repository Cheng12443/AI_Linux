/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kai_cache.h — 多级缓存系统
 *
 * 层次：
 *   L0: CPU 寄存器缓存（编译器优化）
 *   L1: 堆内缓存（struct kmem_cache）
 *   L2: 页缓存（页粒度）
 *   L3: 磁盘缓存（持久化）
 */

#ifndef _KAI_CACHE_H
#define _KAI_CACHE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>

#define KAI_CACHE_NAME_LEN 64
#define KAI_CACHE_MAX_ENTRIES 4096
#define KAI_CACHE_DEFAULT_TTL_MS 5000

/* 缓存条目状态 */
#define KAI_CACHE_VALID   0x01
#define KAI_CACHE_DIRTY   0x02
#define KAI_CACHE_LOCKED  0x04

/* 缓存策略 */
#define KAI_CACHE_LRU     0  /* 最近最少使用 */
#define KAI_CACHE_LFU     1  /* 最少使用频率 */
#define KAI_CACHE_FIFO    2  /* 先进先出 */
#define KAI_CACHE_TTL     3  /* 基于时间过期 */

/* 缓存条目 */
struct kai_cache_entry {
    char             key[KAI_CACHE_NAME_LEN];
    void            *data;
    size_t           data_size;
    __u64            created_ns;
    __u64            last_access_ns;
    __u32            hit_count;
    __u32            flags;
    __u32            ttl_ms;
    struct list_head lru_node;
    struct hlist_node hash_node;
};

/* 缓存统计 */
struct kai_cache_stats {
    __u64 hits;
    __u64 misses;
    __u64 evictions;
    __u64 expirations;
    __u64 inserts;
    __u64 total_size;
};

/* 缓存描述符 */
struct kai_cache {
    char             name[KAI_CACHE_NAME_LEN];
    __u32            max_entries;
    __u32            policy;          /* KAI_CACHE_* */
    __u32            default_ttl_ms;
    __u32            entry_size;      /* 每个条目的固定大小（0=变长）*/

    struct kmem_cache *entry_cache;   /* slab 缓存 */
    spinlock_t        lock;
    struct list_head  lru_list;       /* LRU 链表（最近使用在头）*/
    struct hlist_head *hash_table;    /* 哈希表 */
    __u32             hash_bits;

    struct kai_cache_stats stats;

    /* 回调 */
    void (*on_evict)(struct kai_cache_entry *entry);
    void (*on_expire)(struct kai_cache_entry *entry);
};

/* =========================================================================
 * 公共 API
 * ========================================================================= */

/* 创建 / 销毁 */
struct kai_cache *kai_cache_create(const char *name,
                                   __u32 max_entries,
                                   __u32 policy,
                                   __u32 entry_size,
                                   __u32 default_ttl_ms);
void kai_cache_destroy(struct kai_cache *cache);

/* 存取 */
int kai_cache_put(struct kai_cache *cache,
                  const char *key,
                  const void *data, size_t data_size);
int kai_cache_put_ex(struct kai_cache *cache,
                     const char *key,
                     const void *data, size_t data_size,
                     __u32 ttl_ms);
int kai_cache_get(struct kai_cache *cache,
                  const char *key,
                  void *out, size_t *out_size);
int kai_cache_get_copy(struct kai_cache *cache,
                        const char *key,
                        void **out, size_t *out_size);
int kai_cache_remove(struct kai_cache *cache, const char *key);
int kai_cache_has(struct kai_cache *cache, const char *key);

/* 批量操作 */
int kai_cache_flush(struct kai_cache *cache);
int kai_cache_flush_expired(struct kai_cache *cache);

/* 统计 */
void kai_cache_stats(struct kai_cache *cache, struct kai_cache_stats *out);
void kai_cache_reset_stats(struct kai_cache *cache);

/* 迭代 */
typedef void (*kai_cache_iter_fn)(struct kai_cache_entry *entry, void *priv);
int kai_cache_foreach(struct kai_cache *cache, kai_cache_iter_fn fn, void *priv);

/* 工具：计算缓存键 */
static inline __u32 kai_cache_key_hash(const char *key)
{
    __u32 hash = 5381;
    while (*key)
        hash = ((hash << 5) + hash) + *key++;
    return hash;
}

/* 工具：检查是否过期 */
static inline bool kai_cache_entry_expired(struct kai_cache_entry *entry)
{
    if (!(entry->flags & KAI_CACHE_VALID))
        return true;

    __u64 now = ktime_get_ns();
    __u64 age_ms = (now - entry->last_access_ns) / 1000000ULL;
    return age_ms > entry->ttl_ms;
}

#endif /* _KAI_CACHE_H */
