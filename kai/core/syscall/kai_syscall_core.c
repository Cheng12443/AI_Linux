// SPDX-License-Identifier: GPL-2.0
/*
 * kai_syscall_core.c — sys_infer() 内核内置实现（编译进 vmlinux）
 *
 * 设计原则：
 *   1. 本文件【编入内核】而非模块 → syscall 表静态注册，最正统、可进主线
 *   2. 与具体推理后端【解耦】：推理引擎通过 kai_engine 注册表挂载
 *      - ai_core 模块 insmod 时注册自己的 engine
 *      - 没注册 → 返回 -ENODEV（系统调用本身永远可用）
 *      → "syscall 常驻内核 + AI 后端可热插拔"
 *   3. 参数结构/返回码与用户态封装（libc/syscall()）保持一致
 *
 * 集成步骤见: kai/core/syscall/README.md（patch 脚本自动完成）
 */

#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/capability.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/mutex.h>

/* =========================================================================
 * 版本/常量
 * ========================================================================= */
#define KAI_SYS_VERSION  "1.0.0"
#define KAI_MAX_INOUT    (PAGE_SIZE * 64)   /* 输入/输出上限 256KB */
#define KAI_MAX_CONCURRENT 1024
#define KAI_MODEL_NAME_MAX 64

/* =========================================================================
 * ABI —— 与用户态保持一致
 * ========================================================================= */

/* flags */
#define KAI_INFER_SYNC       0x01
#define KAI_INFER_ASYNC      0x02
#define KAI_INFER_BATCH      0x04
#define KAI_INFER_NOCOPY     0x08
#define KAI_INFER_PINNED     0x10

struct kai_infer_args {
	__u64 model_name_ptr;
	__u64 input_ptr;
	__u64 input_size;
	__u64 output_ptr;
	__u64 output_size;
	__u64 flags;
	__u64 request_id;       /* out */
	__u64 latency_ns;       /* out */
};

/* =========================================================================
 * 推理引擎注册表（后端热插拔）
 * ========================================================================= */

struct kai_engine_ops {
	const char *name;
	int  (*infer)(const void *input, size_t in_sz,
		      void *output, size_t out_sz);
	void (*stats)(u64 *calls, u64 *errors, u64 *latency_ns);
};

static const struct kai_engine_ops *kai_engine;   /* 当前激活引擎 */
static DEFINE_MUTEX(kai_engine_lock);

int kai_engine_register(const struct kai_engine_ops *ops)
{
	if (!ops || !ops->infer)
		return -EINVAL;
	mutex_lock(&kai_engine_lock);
	if (kai_engine)
		pr_warn("kai_sys: engine %s 覆盖 %s\n",
			ops->name, kai_engine->name ?: "none");
	kai_engine = ops;
	mutex_unlock(&kai_engine_lock);
	pr_info("kai_sys: 推理引擎注册: %s\n", ops->name);
	return 0;
}
EXPORT_SYMBOL_GPL(kai_engine_register);

int kai_engine_unregister(const struct kai_engine_ops *ops)
{
	mutex_lock(&kai_engine_lock);
	if (kai_engine == ops)
		kai_engine = NULL;
	mutex_unlock(&kai_engine_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(kai_engine_unregister);

/* 供 ai_core / kai_infer 等模块直接调用的内核态入口 */
int kai_engine_infer(const void *in, size_t in_sz, void *out, size_t out_sz)
{
	const struct kai_engine_ops *e;
	int ret = -ENODEV;

	mutex_lock(&kai_engine_lock);
	e = kai_engine;
	if (e)
		ret = e->infer(in, in_sz, out, out_sz);
	mutex_unlock(&kai_engine_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(kai_engine_infer);

/* =========================================================================
 * 鉴权 / 限流
 * ========================================================================= */

static DEFINE_RATELIMIT_STATE(kai_sys_rl, 10 * HZ, 200);   /* 10s/200 */
static atomic_t kai_active = ATOMIC_INIT(0);
static atomic64_t kai_next_id = ATOMIC64_INIT(1);

static int kai_check_perm(void)
{
	if (!capable(CAP_SYS_ADMIN) && !capable(CAP_SYS_NICE))
		return -EPERM;
	if (!__ratelimit(&kai_sys_rl))
		return -EAGAIN;
	if (atomic_inc_return(&kai_active) > KAI_MAX_CONCURRENT) {
		atomic_dec(&kai_active);
		return -EAGAIN;
	}
	return 0;
}

static void kai_put_perm(void)
{
	atomic_dec(&kai_active);
}

/* =========================================================================
 * 内存安全
 * ========================================================================= */

static int kai_check_user(const void __user *p, size_t n)
{
	if (!p || n == 0 || n > KAI_MAX_INOUT)
		return -EINVAL;
	if (!access_ok(p, n))
		return -EFAULT;
	return 0;
}

static void *kai_copy_in(const void __user *p, size_t n)
{
	void *kbuf = kvmalloc(n, GFP_KERNEL | __GFP_ZERO);
	if (!kbuf)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(kbuf, p, n)) {
		kvfree(kbuf);
		return ERR_PTR(-EFAULT);
	}
	return kbuf;
}

static long kai_copy_out(void __user *dst, size_t cap, const void *src, size_t n)
{
	size_t m = min(cap, n);
	if (!dst)
		return -EINVAL;
	if (copy_to_user(dst, src, m))
		return -EFAULT;
	return (long)m;
}

/* =========================================================================
 * 系统调用本体
 * ========================================================================= */

SYSCALL_DEFINE1(kai_infer, struct kai_infer_args __user *, uargs)
{
	struct kai_infer_args a;
	char model[KAI_MODEL_NAME_MAX];
	void *inbuf = NULL, *outbuf = NULL;
	u64 t0;
	int ret;
	long written = 0;

	/* 1. 参数与权限 */
	if (!uargs)
		return -EINVAL;
	if (copy_from_user(&a, uargs, sizeof(a)))
		return -EFAULT;

	ret = kai_check_perm();
	if (ret)
		return ret;

	/* 2. 模型名 */
	if (strncpy_from_user(model,
			      (const char __user *)a.model_name_ptr,
			      KAI_MODEL_NAME_MAX - 1) < 0) {
		ret = -EFAULT;
		goto out_perm;
	}
	model[KAI_MODEL_NAME_MAX - 1] = '\0';

	/* 3. 输入 */
	ret = kai_check_user((const void __user *)a.input_ptr, a.input_size);
	if (ret)
		goto out_perm;
	inbuf = kai_copy_in((const void __user *)a.input_ptr, a.input_size);
	if (IS_ERR(inbuf)) {
		ret = PTR_ERR(inbuf);
		goto out_perm;
	}

	/* 4. 输出缓冲（输出可为空 → 纯触发/日志模式） */
	if (a.output_ptr && a.output_size) {
		outbuf = kvmalloc(a.output_size, GFP_KERNEL | __GFP_ZERO);
		if (!outbuf) {
			ret = -ENOMEM;
			goto out_in;
		}
	}

	/* 5. 推理（引擎可为空 → -ENODEV，表示后端未加载） */
	t0 = ktime_get_ns();
	/* model 参数暂用于扩展（引擎级模型选择后续加），此刻交给引擎 */
	ret = kai_engine_infer(inbuf, a.input_size,
			       outbuf ? outbuf : inbuf,
			       outbuf ? a.output_size : 0);
	a.latency_ns = ktime_get_ns() - t0;
	a.request_id = atomic64_inc_return(&kai_next_id);

	if (ret == 0 && outbuf && a.output_ptr) {
		written = kai_copy_out((void __user *)a.output_ptr,
				       a.output_size, outbuf, a.output_size);
		if (written < 0) {
			ret = (int)written;
			goto out_out;
		}
		ret = 0;
		written = (long)a.output_size;
	}

	/* 回写元数据 */
	(void)copy_to_user(&uargs->request_id, &a.request_id,
			   sizeof(a.request_id));
	(void)copy_to_user(&uargs->latency_ns, &a.latency_ns,
			   sizeof(a.latency_ns));

out_out:
	if (outbuf)
		kvfree(outbuf);
out_in:
	if (inbuf && !IS_ERR(inbuf))
		kvfree(inbuf);
out_perm:
	kai_put_perm();
	return ret ? ret : written;
}
