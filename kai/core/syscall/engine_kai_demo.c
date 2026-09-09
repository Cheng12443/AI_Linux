// SPDX-License-Identifier: GPL-2.0
/*
 * engine_kai_demo.c — sys_infer() 推理引擎演示模块
 *
 * 独立 .ko：加载时向内核注册"本地规则引擎"，
 * 之后任何进程调用 sys_infer(548) 都能拿到结果（不进外部 API）。
 * ai_core/kai_infer 模块可参照本文件注册自己的引擎。
 *
 * 编译（内核树内）：
 *   make -C $KERNEL_SRC M=$PWD modules
 *
 * 加载/验证：
 *   insmod engine_kai_demo.ko
 *   dmesg | tail -1                    # 引擎注册成功
 *   gcc test_sys_infer.c -o /tmp/tsi && /tmp/tsi
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

/* 内核内置 syscall 侧导出的接口（见 kai_syscall_core.c）*/
extern int kai_engine_register(const struct kai_engine_ops *ops);
extern int kai_engine_unregister(const struct kai_engine_ops *ops);

struct kai_engine_ops {
	const char *name;
	int  (*infer)(const void *input, size_t in_sz,
		      void *output, size_t out_sz);
	void (*stats)(u64 *calls, u64 *errors, u64 *latency_ns);
};

static u64 demo_calls, demo_errors;

/*
 * 本地规则推理：
 *   - 输入按 float 数组解释
 *   - 输出: float[0] = 特征均值归一化(0~1)，作为置信/异常评分
 *   真实引擎这里应接 量化模型/Layer2(DeepSeek/Kimi)
 */
static int demo_infer(const void *input, size_t in_sz,
		      void *output, size_t out_sz)
{
	const float *feat = input;
	int n = in_sz / sizeof(float);
	float *out = output;

	if (!input || !output || n < 1)
		return -EINVAL;
	if (out_sz < sizeof(float))
		return -ENOSPC;

	/* 简易评分：均值归一（演示）*/
	float sum = 0;
	int i;
	for (i = 0; i < n && i < 64; i++)
		sum += feat[i];
	float score = (n ? sum / n : 0.5f);
	if (score < 0) score = 0;
	if (score > 1) score = 1;

	*out = score;
	if (out_sz >= sizeof(float) * 2)
		out[1] = 1.0f - score;      /* 互补类 */
	demo_calls++;
	return 0;
}

static void demo_stats(u64 *calls, u64 *errors, u64 *latency_ns)
{
	if (calls) *calls = demo_calls;
	if (errors) *errors = demo_errors;
	if (latency_ns) *latency_ns = 0;
}

static const struct kai_engine_ops demo_engine = {
	.name  = "demo_local_rule",
	.infer = demo_infer,
	.stats = demo_stats,
};

static int __init engine_demo_init(void)
{
	int r = kai_engine_register(&demo_engine);
	if (r)
		pr_err("engine_demo: register failed %d\n", r);
	else
		pr_info("engine_demo: 引擎已注册。调用 syscall 548 即可推理。\n");
	return r;
}

static void __exit engine_demo_exit(void)
{
	kai_engine_unregister(&demo_engine);
	pr_info("engine_demo: 引擎已注销\n");
}

module_init(engine_demo_init);
module_exit(engine_demo_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("sys_infer() demo inference engine");
MODULE_VERSION("1.0.0");
