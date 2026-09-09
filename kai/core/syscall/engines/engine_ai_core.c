// SPDX-License-Identifier: GPL-2.0
/*
 * engine_ai_core.c — 把 ai_core（Layer1 + Layer2 全链路）注册为 sys_infer 引擎
 *
 * 依赖（需已加载）：
 *   ai_core.ko   → 提供 ai_infer_sync()
 *   本模块 .ko   → 调用 kai_engine_register() 接入 syscall 548
 *
 * 加载顺序：
 *   insmod ai_core.ko
 *   insmod engine_ai_core.ko
 *   之后任何进程 sys_infer(548) → ai_infer_sync → 缓存/量化/Layer2(DeepSeek/Kimi)
 *
 * 默认模型名：module 参数 default_model="deepseek-chat"
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

/* ---- 内核内置 syscall 侧接口（kai_syscall_core.c） ---- */
struct kai_engine_ops {
	const char *name;
	int  (*infer)(const void *input, size_t in_sz,
		      void *output, size_t out_sz);
	void (*stats)(u64 *calls, u64 *errors, u64 *latency_ns);
};
extern int kai_engine_register(const struct kai_engine_ops *ops);
extern int kai_engine_unregister(const struct kai_engine_ops *ops);

/* ---- ai_core 导出接口（ai_core.ko） ---- */
extern int ai_infer_sync(const char *model_name,
			 const void *input, size_t isize,
			 void *output, size_t osize,
			 u64 *latency_ns);

static char *default_model = "deepseek-chat";
module_param(default_model, charp, 0644);
MODULE_PARM_DESC(default_model, "sys_infer 使用的默认模型名");

static u64 e_calls, e_errors, e_latency;

static int ai_core_infer(const void *input, size_t in_sz,
			 void *output, size_t out_sz)
{
	u64 lat = 0;
	int r = ai_infer_sync(default_model, input, in_sz,
			      output, out_sz, &lat);
	e_calls++;
	if (r)
		e_errors++;
	e_latency += lat;
	return r;
}

static void ai_core_stats(u64 *calls, u64 *errors, u64 *latency_ns)
{
	if (calls)      *calls = e_calls;
	if (errors)     *errors = e_errors;
	if (latency_ns) *latency_ns = e_latency;
}

static const struct kai_engine_ops ai_core_engine = {
	.name  = "ai_core",
	.infer = ai_core_infer,
	.stats = ai_core_stats,
};

static int __init engine_ai_core_init(void)
{
	int r = kai_engine_register(&ai_core_engine);
	if (r)
		pr_err("engine_ai_core: register failed %d\n", r);
	else
		pr_info("engine_ai_core: syscall548 → ai_core(model=%s)\n",
			default_model);
	return r;
}

static void __exit engine_ai_core_exit(void)
{
	kai_engine_unregister(&ai_core_engine);
	pr_info("engine_ai_core: unregistered\n");
}

module_init(engine_ai_core_init);
module_exit(engine_ai_core_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("sys_infer() engine adapter -> ai_core (Layer1+Layer2)");
MODULE_VERSION("1.0.0");
