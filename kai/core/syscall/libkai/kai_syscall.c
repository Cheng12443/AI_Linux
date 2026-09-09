// SPDX-License-Identifier: GPL-2.0
/*
 * kai_syscall.c — libkai 实现
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "kai_syscall.h"

const char *kai_strerr(long ret)
{
	if (ret >= 0)
		return "ok";
	switch (-ret) {
	case 0:            return "ok";
	case EPERM:        return "permission denied (need CAP_SYS_ADMIN/NICE)";
	case EINVAL:       return "invalid argument (size/pointer)";
	case EFAULT:       return "bad user pointer";
	case ENOMEM:       return "out of kernel memory";
	case EAGAIN:       return "rate limited / too many concurrent";
	case ENODEV:       return "no inference engine registered (insmod one)";
	case ENOENT:       return "model not found";
	case ENOSPC:       return "output buffer too small";
	default:           return strerror(-ret);
	}
}

long kai_syscall(struct kai_infer_args *args)
{
	return syscall(__NR_kai_infer, args);
}

long kai_infer(const char *model,
	       const void *input, size_t in_sz,
	       void *output, size_t out_sz,
	       uint64_t *req_id, uint64_t *lat_ns)
{
	struct kai_infer_args a;

	if ((!input || in_sz == 0 || in_sz > KAI_MAX_IO) ||
	    (!output || out_sz == 0 || out_sz > KAI_MAX_IO) ||
	    (model && strlen(model) >= KAI_MODEL_NAME_MAX))
		return -EINVAL;

	memset(&a, 0, sizeof(a));
	a.model_name_ptr = (uint64_t)(uintptr_t)(model ? model : "");
	a.input_ptr      = (uint64_t)(uintptr_t)input;
	a.input_size     = in_sz;
	a.output_ptr     = (uint64_t)(uintptr_t)output;
	a.output_size    = out_sz;
	a.flags          = KAI_INFER_SYNC;

	long r = kai_syscall(&a);
	if (r >= 0) {
		if (req_id) *req_id = a.request_id;
		if (lat_ns) *lat_ns = a.latency_ns;
	}
	return r;
}

long kai_infer_floats(const char *model,
		      const float *feat, size_t n_feat,
		      float *out, size_t max_out,
		      uint64_t *req_id, uint64_t *lat_ns)
{
	if (!feat || n_feat == 0 || !out || max_out == 0)
		return -EINVAL;
	/* 转成字节视图，要求字节数不超上限 */
	size_t in_bytes = n_feat * sizeof(float);
	if (in_bytes > KAI_MAX_IO)
		return -EINVAL;
	return kai_infer(model, feat, in_bytes, out, max_out * sizeof(float),
			 req_id, lat_ns);
}
