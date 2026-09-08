/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kai_net_opt.h — 网络优化接口
 */

#ifndef _KAI_NET_OPT_H
#define _KAI_NET_OPT_H

#include <linux/types.h>

/* DNS 缓存 */
int kai_dns_cache_lookup(const char *hostname, __u32 *ipv4);
int kai_dns_cache_insert(const char *hostname, __u32 ipv4);
int kai_dns_cache_flush(void);
void kai_dns_cache_stats(int *count, int *hits, int *misses);

/* 连接复用 */
struct kai_conn;
struct kai_conn *kai_conn_get(const char *host, int port);
int kai_conn_put(struct kai_conn *conn);

#endif /* _KAI_NET_OPT_H */
