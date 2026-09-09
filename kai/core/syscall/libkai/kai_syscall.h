// SPDX-License-Identifier: GPL-2.0
/*
 * kai_syscall.h — libkai：sys_infer() 用户态库
 * 与内核 kai_syscall_core.c 的 ABI 严格一致
 */

#ifndef _LIBKAI_H
#define _LIBKAI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 系统调用号 */
#ifndef __NR_kai_infer
#define __NR_kai_infer 548
#endif

/* flags（与内核一致）*/
#define KAI_INFER_SYNC   0x01u
#define KAI_INFER_ASYNC  0x02u
#define KAI_INFER_BATCH  0x04u
#define KAI_INFER_NOCOPY 0x08u
#define KAI_INFER_PINNED 0x10u

/* 上限 */
#define KAI_MAX_IO        (256u * 1024u)
#define KAI_MODEL_NAME_MAX 64u

/* 参数块（严格对应内核 struct kai_infer_args）*/
struct kai_infer_args {
	uint64_t model_name_ptr;
	uint64_t input_ptr;
	uint64_t input_size;
	uint64_t output_ptr;
	uint64_t output_size;
	uint64_t flags;
	uint64_t request_id;   /* out */
	uint64_t latency_ns;   /* out */
};

/* 错误码 -> 可读串 */
const char *kai_strerr(long ret);

/* 同步推理（最简用法）：返回 >0 写入字节，<0 为 -errno */
long kai_infer(const char *model,
	       const void *input, size_t in_sz,
	       void *output, size_t out_sz,
	       uint64_t *req_id, uint64_t *lat_ns);

/* float 特征快捷推理（常用于打分）*/
long kai_infer_floats(const char *model,
		      const float *feat, size_t n_feat,
		      float *out, size_t max_out,
		      uint64_t *req_id, uint64_t *lat_ns);

/* 裸系统调用 */
long kai_syscall(struct kai_infer_args *args);

#ifdef __cplusplus
}
#endif
#endif /* _LIBKAI_H */
