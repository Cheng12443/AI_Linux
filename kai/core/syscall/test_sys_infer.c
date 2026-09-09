// SPDX-License-Identifier: GPL-2.0
/*
 * test_sys_infer.c — sys_infer() 用户态冒烟测试
 *
 * 编译： gcc test_sys_infer.c -o /tmp/tsi
 * 运行： /tmp/tsi            # 需 root / CAP_SYS_ADMIN 或 CAP_SYS_NICE
 *
 * 依赖：内核已集成 syscall 548 + 至少一个推理引擎已注册
 *       （没有引擎时预期返回 -ENODEV，属正常：先 insmod engine_kai_demo.ko）
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __NR_kai_infer
#define __NR_kai_infer 548
#endif

/* 与内核 kai_syscall_core.c 的 ABI 保持一致 */
struct kai_infer_args {
	uint64_t model_name_ptr;
	uint64_t input_ptr;
	uint64_t input_size;
	uint64_t output_ptr;
	uint64_t output_size;
	uint64_t flags;       /* bit0=sync */
	uint64_t request_id;  /* out */
	uint64_t latency_ns;  /* out */
};

int main(void)
{
	float in[8] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f };
	float out[4] = { 0 };
	char model[] = "demo_local_rule";

	struct kai_infer_args a;
	memset(&a, 0, sizeof(a));
	a.model_name_ptr = (uint64_t)(uintptr_t)model;
	a.input_ptr      = (uint64_t)(uintptr_t)in;
	a.input_size     = sizeof(in);
	a.output_ptr     = (uint64_t)(uintptr_t)out;
	a.output_size    = sizeof(out);
	a.flags          = 1;  /* SYNC */

	printf("== sys_infer() smoke test ==\n");
	printf("syscall nr : %d\n", __NR_kai_infer);

	long r = syscall(__NR_kai_infer, &a);

	if (r < 0) {
		printf("FAIL: ret=%ld errno=%d (%s)\n",
		       r, errno, strerror(errno));
		printf("  提示: -ENODEV=引擎未加载，先 insmod engine_kai_demo.ko\n");
		printf("        -EPERM=需要 root / CAP_SYS_ADMIN\n");
		return 1;
	}

	printf("OK   : 写入 %ld 字节\n", r);
	printf("score: %.4f  (0~1，演示评分)\n", out[0]);
	printf("id   : %llu\n", (unsigned long long)a.request_id);
	printf("lat  : %llu ns (%.2f ms)\n",
	       (unsigned long long)a.latency_ns,
	       (double)a.latency_ns / 1e6);
	return 0;
}
