/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kai_mem_opt.h — 内存优化接口
 */

#ifndef _KAI_MEM_OPT_H
#define _KAI_MEM_OPT_H

#include <linux/types.h>

/* 页面预取 */
void kai_record_page_access(unsigned long pfn);
int  kai_predict_hot_pages(unsigned long *pfns, int max_pages);
int  kai_prefetch_pages(unsigned long *pfns, int count);

/* 大页 */
void *kai_alloc_huge_weights(size_t size);
void  kai_free_huge_weights(void *addr);

/* LSM 决策缓存 */
int kai_lsm_cache_lookup(__u32 inode_hash, __u32 *decision);
int kai_lsm_cache_insert(__u32 inode_hash, __u32 decision);

/* 内存压缩 */
int kai_zswap_enable(void);

#endif /* _KAI_MEM_OPT_H */
