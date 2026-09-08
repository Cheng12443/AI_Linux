// SPDX-License-Identifier: GPL-2.0
/*
 * kai_net_opt.c — 网络优化（ROADMAP 1.3）
 *
 * 功能：
 *   - DNS 缓存（避免重复 DNS 查询）
 *   - 连接复用（HTTP keep-alive）
 *   - XDP 批量推理
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/net.h>
#include <linux/inet.h>
#include <linux/socket.h>

#include "kai_net_opt.h"

#define DRV_NAME "kai_net_opt"
#define DRV_VER  "1.0.0"

/* =========================================================================
 * DNS 缓存
 * ========================================================================= */

#define DNS_CACHE_SIZE  1024
#define DNS_CACHE_BITS  10
#define DNS_TTL_MS      300000  /* 5 分钟 */

struct dns_entry {
    char             hostname[256];
    __u32            ipv4;
    __u64            expires_ns;
    struct hlist_node node;
    struct list_head lru;
};

static DEFINE_HASHTABLE(dns_table, DNS_CACHE_BITS);
static LIST_HEAD(dns_lru);
static DEFINE_SPINLOCK(dns_lock);
static atomic_t dns_count = ATOMIC_INIT(0);

/*
 * kai_dns_cache_lookup — 查询 DNS 缓存
 */
int kai_dns_cache_lookup(const char *hostname, __u32 *ipv4)
{
    struct dns_entry *entry;
    __u64 now = ktime_get_ns();
    u32 hash;

    if (!hostname || !ipv4)
        return -EINVAL;

    hash = full_name_hash(NULL, hostname, strlen(hostname));

    spin_lock(&dns_lock);
    hash_for_each_possible(dns_table, entry, node, hash) {
        if (strcmp(entry->hostname, hostname) == 0) {
            if (now < entry->expires_ns) {
                *ipv4 = entry->ipv4;
                /* 移到 LRU 头部 */
                list_move(&entry->lru, &dns_lru);
                spin_unlock(&dns_lock);
                return 0;
            }
        }
    }
    spin_unlock(&dns_lock);
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(kai_dns_cache_lookup);

/*
 * kai_dns_cache_insert — 插入 DNS 缓存
 */
int kai_dns_cache_insert(const char *hostname, __u32 ipv4)
{
    struct dns_entry *entry;
    u32 hash;

    if (!hostname)
        return -EINVAL;

    hash = full_name_hash(NULL, hostname, strlen(hostname));

    /* 检查是否已存在 */
    spin_lock(&dns_lock);
    hash_for_each_possible(dns_table, entry, node, hash) {
        if (strcmp(entry->hostname, hostname) == 0) {
            entry->ipv4 = ipv4;
            entry->expires_ns = ktime_get_ns() + DNS_TTL_MS * 1000000ULL;
            spin_unlock(&dns_lock);
            return 0;
        }
    }

    /* 容量检查 */
    if (atomic_read(&dns_count) >= DNS_CACHE_SIZE) {
        /* 驱逐最旧条目 */
        entry = list_last_entry(&dns_lru, struct dns_entry, lru);
        hash_del(&entry->node);
        list_del(&entry->lru);
        kfree(entry);
        atomic_dec(&dns_count);
    }

    entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
    if (!entry) {
        spin_unlock(&dns_lock);
        return -ENOMEM;
    }

    strscpy(entry->hostname, hostname, sizeof(entry->hostname));
    entry->ipv4 = ipv4;
    entry->expires_ns = ktime_get_ns() + DNS_TTL_MS * 1000000ULL;

    hash_add(dns_table, &entry->node, hash);
    list_add(&entry->lru, &dns_lru);
    atomic_inc(&dns_count);

    spin_unlock(&dns_lock);
    return 0;
}
EXPORT_SYMBOL_GPL(kai_dns_cache_insert);

/*
 * kai_dns_cache_flush — 清空 DNS 缓存
 */
int kai_dns_cache_flush(void)
{
    struct dns_entry *entry;
    struct hlist_node *tmp;
    int bkt;

    spin_lock(&dns_lock);
    hash_for_each_safe(dns_table, bkt, tmp, entry, node) {
        hash_del(&entry->node);
        list_del(&entry->lru);
        kfree(entry);
        atomic_dec(&dns_count);
    }
    spin_unlock(&dns_lock);

    return 0;
}
EXPORT_SYMBOL_GPL(kai_dns_cache_flush);

/*
 * kai_dns_cache_stats — DNS 缓存统计
 */
void kai_dns_cache_stats(int *count, int *hits, int *misses)
{
    if (count)
        *count = atomic_read(&dns_count);
    /* hits/misses 需要额外跟踪，这里简化 */
    if (hits) *hits = 0;
    if (misses) *misses = 0;
}
EXPORT_SYMBOL_GPL(kai_dns_cache_stats);

/* =========================================================================
 * 连接复用（HTTP keep-alive 连接池）
 * ========================================================================= */

#define CONN_POOL_SIZE 64

struct conn_entry {
    char        host[256];
    int         port;
    void       *socket;      /* struct socket * */
    __u64       last_used_ns;
    __u32       used_count;
    bool        in_use;
    struct list_head node;
};

static LIST_HEAD(conn_pool);
static DEFINE_SPINLOCK(conn_lock);
static atomic_t conn_count = ATOMIC_INIT(0);

/*
 * kai_conn_get — 获取复用的连接
 */
struct kai_conn *kai_conn_get(const char *host, int port)
{
    struct conn_entry *entry;

    spin_lock(&conn_lock);
    list_for_each_entry(entry, &conn_pool, node) {
        if (!entry->in_use && strcmp(entry->host, host) == 0 &&
            entry->port == port) {
            entry->in_use = true;
            entry->used_count++;
            spin_unlock(&conn_lock);
            return (struct kai_conn *)entry;
        }
    }
    spin_unlock(&conn_lock);
    return NULL;
}
EXPORT_SYMBOL_GPL(kai_conn_get);

/*
 * kai_conn_put — 归还连接（复用）
 */
int kai_conn_put(struct kai_conn *conn)
{
    struct conn_entry *entry = (struct conn_entry *)conn;

    if (!entry)
        return -EINVAL;

    spin_lock(&conn_lock);
    entry->in_use = false;
    entry->last_used_ns = ktime_get_ns();
    spin_unlock(&conn_lock);
    return 0;
}
EXPORT_SYMBOL_GPL(kai_conn_put);

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init kai_net_opt_init(void)
{
    pr_info("========================================\n");
    pr_info("  KAI Net Opt v%s\n", DRV_VER);
    pr_info("  DNS cache + Connection pool\n");
    pr_info("========================================\n");

    hash_init(dns_table);
    INIT_LIST_HEAD(&dns_lru);
    INIT_LIST_HEAD(&conn_pool);

    return 0;
}

static void __exit kai_net_opt_exit(void)
{
    struct dns_entry *entry;
    struct hlist_node *tmp;
    int bkt;

    /* 清理 DNS 缓存 */
    hash_for_each_safe(dns_table, bkt, tmp, entry, node) {
        hash_del(&entry->node);
        kfree(entry);
    }

    pr_info("%s: unloaded\n", DRV_NAME);
}

module_init(kai_net_opt_init);
module_exit(kai_net_opt_exit);

MODULE_DESCRIPTION("KAI Net Opt — DNS cache & connection pool");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
