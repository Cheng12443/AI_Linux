// SPDX-License-Identifier: GPL-2.0
/* demo.c — libkai 最小演示 */
#include <stdio.h>
#include "kai_syscall.h"

int main(void)
{
	float feat[4] = { 0.2f, 0.3f, 0.4f, 0.5f };
	float out[2]  = { 0 };
	uint64_t id = 0, lat = 0;

	long n = kai_infer_floats("demo_local_rule", feat, 4, out, 2, &id, &lat);
	if (n < 0) {
		printf("kai_infer failed: %ld -> %s\n", n, kai_strerr(n));
		printf("（需要内核已集成 syscall548 且引擎已加载）\n");
		return 1;
	}
	printf("libkai ok: score=%.3f  id=%llu  latency=%lluns\n",
	       out[0], (unsigned long long)id, (unsigned long long)lat);
	return 0;
}
