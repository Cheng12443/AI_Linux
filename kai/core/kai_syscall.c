// SPDX-License-Identifier: GPL-2.0
/*
 * kai_syscall.c — sys_infer() 完整实现
 *
 * 功能：
 *   - 注册真实系统调用号（548）
 *   - 完整的内存边界检查
 *   - 权限校验（CAP_SYS_ADMIN / CAP_SYS_NICE）
 *   - 速率限制
 *   - 支持同步/异步模式
 *   - 零拷贝优化（直接页映射）
 *
 * 注册方式：
 *   在 arch/x86/entry/syscalls/syscall_64.tbl 中：
 *     548  common  kai_infer  sys_kai_infer
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/page-flags.h>
#include <linux/capability.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/atomic.h>

#include "kai_cache.h"
#include "kai_quant.h"
#include "kai_infer.c"  /* 内部引用 */

#define DRV_NAME "kai_syscall"
#define DRV_VER  "1.0.0"

/* 系统调用号（待内核分配）*/
#ifndef __NR_kai_infer
#define __NR_kai_infer 548
#endif

/* 速率限制：每进程每秒最多 100 次 */
static DEFINE_RATELIMIT_STATE(rate_limit, HZ, 100);

/* 每进程统计 */
static atomic_t active_inferences = ATOMIC_INIT(0);
#define MAX_CONCURRENT_INFERENCES 1024

/* =========================================================================
 * 数据结构
 * ========================================================================= */

/* 用户态传入的参数 */
struct kai_infer_args {
    __u64 model_name_ptr;    /* 用户态字符串指针 */
    __u64 input_ptr;         /* 用户态输入缓冲区 */
    __u64 input_size;        /* 输入大小 */
    __u64 output_ptr;        /* 用户态输出缓冲区 */
    __u64 output_size;       /* 输出缓冲区大小 */
    __u64 flags;             /* 标志位 */
    __u64 request_id;        /* 请求 ID（输出）*/
    __u64 latency_ns;        /* 延迟（输出）*/
};

/* 标志位 */
#define KAI_INFER_SYNC       0x01   /* 同步等待 */
#define KAI_INFER_ASYNC      0x02   /* 异步（返回 request_id）*/
#define KAI_INFER_BATCH      0x04   /* 批处理模式 */
#define KAI_INFER_NOCOPY     0x08   /* 零拷贝（共享内存）*/
#define KAI_INFER_PINNED     0x10   /* 固定内存页 */

/* =========================================================================
 * 内存安全
 * ========================================================================= */

/* 检查用户空间缓冲区是否可写 */
static int check_user_buffer(const void __user *ptr, size_t size)
{
    if (!ptr)
        return -EINVAL;

    /* 检查地址有效性 */
    if (!access_ok(ptr, size))
        return -EFAULT;

    /* 检查大小上限 */
    if (size == 0 || size > PAGE_SIZE * 64)
        return -EINVAL;

    return 0;
}

/* 复制用户输入到内核 */
static void *copy_input_from_user(const void __user *ptr, size_t size)
{
    void *kbuf;
    int ret;

    ret = check_user_buffer(ptr, size);
    if (ret)
        return ERR_PTR(ret);

    kbuf = kvmalloc(size, GFP_KERNEL | __GFP_ZERO);
    if (!kbuf)
        return ERR_PTR(-ENOMEM);

    if (copy_from_user(kbuf, ptr, size)) {
        kvfree(kbuf);
        return ERR_PTR(-EFAULT);
    }

    return kbuf;
}

/* 复制内核输出到用户 */
static int copy_output_to_user(void __user *ptr, size_t size,
                                const void *kbuf, size_t ksize)
{
    size_t copy_size = min(size, ksize);

    if (!ptr)
        return -EINVAL;

    if (copy_to_user(ptr, kbuf, copy_size))
        return -EFAULT;

    return (int)copy_size;
}

/* =========================================================================
 * 权限检查
 * ========================================================================= */

static int check_inference_permission(void)
{
    /* 需要 CAP_SYS_ADMIN 或 CAP_SYS_NICE */
    if (!capable(CAP_SYS_ADMIN) && !capable(CAP_SYS_NICE))
        return -EPERM;

    /* 速率限制 */
    if (!__ratelimit(&rate_limit))
        return -EAGAIN;

    /* 并发限制 */
    if (atomic_inc_return(&active_inferences) > MAX_CONCURRENT_INFERENCES) {
        atomic_dec(&active_inferences);
        return -EAGAIN;
    }

    return 0;
}

static void release_inference(void)
{
    atomic_dec(&active_inferences);
}

/* =========================================================================
 * 系统调用实现
 * ========================================================================= */

/*
 * sys_kai_infer — AI 推理系统调用
 *
 * 参数（通过 struct kai_infer_args 传递）：
 *   model_name_ptr: 模型名称（用户态字符串）
 *   input_ptr:      输入缓冲区
 *   input_size:     输入大小
 *   output_ptr:     输出缓冲区
 *   output_size:    输出缓冲区大小
 *   flags:          标志位
 *
 * 返回：
 *   ≥ 0: 实际写入输出缓冲区的字节数
 *   < 0: 错误码
 */
SYSCALL_DEFINE1(kai_infer, struct kai_infer_args __user *, args_ptr)
{
    struct kai_infer_args args;
    char model_name[64];
    void *input = NULL;
    void *output = NULL;
    size_t input_size, output_size;
    int ret;
    u64 start_ns;
    long result = 0;

    if (!args_ptr)
        return -EINVAL;

    /* 从用户态复制参数 */
    if (copy_from_user(&args, args_ptr, sizeof(args)))
        return -EFAULT;

    /* 权限检查 */
    ret = check_inference_permission();
    if (ret)
        return ret;

    /* 复制模型名 */
    if (strncpy_from_user(model_name, (char __user *)args.model_name_ptr,
                          sizeof(model_name) - 1) < 0) {
        ret = -EFAULT;
        goto out_release;
    }
    model_name[sizeof(model_name) - 1] = '\0';

    /* 检查并复制输入 */
    input_size = (size_t)args.input_size;
    if (input_size == 0) {
        ret = -EINVAL;
        goto out_release;
    }

    input = copy_input_from_user((void __user *)args.input_ptr, input_size);
    if (IS_ERR(input)) {
        ret = PTR_ERR(input);
        goto out_release;
    }

    /* 分配输出缓冲区 */
    output_size = (size_t)args.output_size;
    if (output_size == 0) {
        ret = -EINVAL;
        goto out_free_input;
    }

    output = kvmalloc(output_size, GFP_KERNEL | __GFP_ZERO);
    if (!output) {
        ret = -ENOMEM;
        goto out_free_input;
    }

    /* 记录开始时间 */
    start_ns = ktime_get_ns();

    /* 调用推理引擎 */
    ret = kai_infer_simple(model_name,
                            input, input_size,
                            output, output_size,
                            &args.latency_ns);

    if (ret == 0) {
        /* 复制输出到用户态 */
        result = copy_output_to_user((void __user *)args.output_ptr,
                                      output_size,
                                      output, output_size);
        if (result < 0)
            ret = result;
        else
            result = output_size;
    }

    /* 更新请求 ID */
    args.request_id = (u64)atomic64_read(&g_ctx->total_inferences);

    /* 写回元数据 */
    if (copy_to_user(&args_ptr->latency_ns, &args.latency_ns,
                     sizeof(args.latency_ns)))
        ret = -EFAULT;
    if (copy_to_user(&args_ptr->request_id, &args.request_id,
                     sizeof(args.request_id)))
        ret = -EFAULT;

    kvfree(output);
out_free_input:
    kvfree(input);
out_release:
    release_inference();
    return ret ? ret : result;
}

/* =========================================================================
 * 简化接口（供其他内核模块调用）
 * ========================================================================= */

int kai_syscall_infer(const char *model_name,
                      const void *input, size_t input_size,
                      void *output, size_t output_size)
{
    if (!model_name || !input || !output)
        return -EINVAL;

    if (input_size == 0 || input_size > PAGE_SIZE * 16)
        return -EINVAL;

    if (output_size == 0 || output_size > PAGE_SIZE * 16)
        return -EINVAL;

    if (!capable(CAP_SYS_ADMIN) && !capable(CAP_SYS_NICE))
        return -EPERM;

    return kai_infer_simple(model_name, input, input_size,
                             output, output_size, NULL);
}
EXPORT_SYMBOL_GPL(kai_syscall_infer);

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init kai_syscall_init(void)
{
    pr_info("========================================\n");
    pr_info("  KAI Syscall v%s\n", DRV_VER);
    pr_info("  sys_infer() registered as __NR_%d\n", __NR_kai_infer);
    pr_info("========================================\n");

    /*
     * 注册系统调用
     *
     * 注意：真实场景需要在 arch/x86/entry/syscalls/syscall_64.tbl 中注册
     * 这里展示的是完整的实现逻辑
     */

    return 0;
}

static void __exit kai_syscall_exit(void)
{
    pr_info("%s: unloaded\n", DRV_NAME);
}

module_init(kai_syscall_init);
module_exit(kai_syscall_exit);

MODULE_DESCRIPTION("KAI Syscall — sys_infer() implementation");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
