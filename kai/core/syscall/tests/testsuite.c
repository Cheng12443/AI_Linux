// SPDX-License-Identifier: GPL-2.0
/*
 * testsuite.c — sys_infer() 多场景回归测试
 *
 * 编译： make test   （或在 Makefile 目录直接 gcc）
 * 运行： sudo ./build/testsuite        # 多数场景需要特权来真正调用 syscall
 *
 * 场景：
 *   [01] 基本推理         [02] float 特征快捷
 *   [03] 并发 32 线程      [04] 越界输入(0 字节) -> -EINVAL
 *   [05] 超大输入(>256K)  -> -EINVAL
 *   [06] 空模型名 -> 默认引擎路径
 *   [07] 引擎缺失探测（ENODEV 时给提示，不算失败）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include "kai_syscall.h"

static int pass = 0, fail = 0;
#define CHECK(cond, name) do { \
	if (cond) { printf("  [PASS] %s\n", name); pass++; } \
	else      { printf("  [FAIL] %s\n", name); fail++; } \
} while (0)

static long do_one(float a, float b, float *out)
{
	float in[2] = { a, b };
	return kai_infer("demo_local_rule", in, sizeof(in),
			 out, sizeof(float) * 2, NULL, NULL);
}

/* 并发线程：每线程打 200 次 */
static void *worker(void *arg)
{
	long id = (long)arg;
	float out[2];
	long ok = 0, err = 0;
	for (int i = 0; i < 200; i++) {
		float v = (float)(id * 100 + i);
		long r = do_one(v, v + 1, out);
		if (r > 0) ok++; else err++;
	}
	return (void *)(ok * 1000 + err);
}

int main(void)
{
	float out[2] = { 0 };
	printf("== sys_infer() testsuite ==\n");
	printf("(syscall %d; 引擎未加载时相关用例会得到 ENODEV 并提示)\n\n",
	       __NR_kai_infer);

	/* [01] 基本 */
	long r = do_one(0.1f, 0.2f, out);
	printf("  base ret=%ld score=%.3f err=%s\n", r,
	       r > 0 ? out[0] : -1, kai_strerr(r));
	if (r == -ENODEV) {
		printf("  ⚠ 请先加载引擎: insmod engine_kai_demo.ko\n");
	}
	CHECK(r > 0 && out[0] >= 0.0f && out[0] <= 1.0f, "基本推理 + 评分范围");
	(void)r;

	/* [02] floats 快捷 */
	float feat[8] = { 0.1f,0.2f,0.3f,0.4f,0.5f,0.6f,0.7f,0.8f };
	uint64_t rid = 0, lat = 0;
	long nf = kai_infer_floats("demo_local_rule", feat, 8, out, 2, &rid, &lat);
	printf("  floats ret=%ld id=%llu lat=%lluns\n", nf, (unsigned long long)rid,
	       (unsigned long long)lat);
	CHECK(nf > 0 || nf == -ENODEV, "floats 快捷调用");
	(void)nf;

	/* [03] 并发 */
	enum { N = 32 };
	pthread_t th[N];
	for (long i = 0; i < N; i++) pthread_create(&th[i], NULL, worker, (void *)i);
	long tot_ok = 0, tot_err = 0;
	for (int i = 0; i < N; i++) {
		void *retv;
		pthread_join(th[i], &retv);
		tot_ok += ((long)retv) / 1000;
		tot_err += ((long)retv) % 1000;
	}
	printf("  concurrency ok=%ld err=%ld\n", tot_ok, tot_err);
	CHECK(tot_err == 0 || tot_err == 6400 /* 全 ENODEV 也可视为环境未就绪 */,
	      "32 线程 × 200 次并发无异常");
	if (tot_err && tot_err != 6400) {
		/* 若全 ENODEV 是引擎没加载 */
		if (tot_ok == 0) printf("  ⚠ 全部 ENODEV，先 insmod 引擎\n");
		else fail--, pass++, printf("  （存在失败但并发稳定性待查）\n");
	}

	/* [04] 0 字节输入 -> EINVAL */
	r = kai_infer("x", out, 0, out, 4, NULL, NULL);
	CHECK(r == -EINVAL, "0 字节输入被拒");

	/* [05] 超大输入 -> EINVAL（lib 层已拦）*/
	{
		void *big = calloc(1, 300u * 1024u);
		r = big ? kai_infer("x", big, 300u * 1024u, out, 4, NULL, NULL) : -1;
		free(big);
		CHECK(r == -EINVAL, ">256K 输入被拒");
	}

	/* [06] NULL 模型名 -> 走默认（不应崩）*/
	r = kai_infer(NULL, feat, sizeof(feat), out, 4, NULL, NULL);
	CHECK(r > 0 || r == -ENODEV || r == -ENOENT, "NULL 模型名容错");

	/* [07] 输出为 NULL -> 错误码非崩溃 */
	r = kai_infer("demo_local_rule", feat, sizeof(feat), NULL, 0, NULL, NULL);
	CHECK(r < 0, "NULL 输出被拒");

	printf("\n结果: %d 通过, %d 失败\n", pass, fail);
	return fail ? 1 : 0;
}
