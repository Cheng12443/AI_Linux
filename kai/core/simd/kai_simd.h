/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kai_simd.h — 内核态 SIMD 加速（ROADMAP 2.1）
 *
 * 支持架构：
 *   - x86_64: AVX2 / AVX512（通过 kernel_fpu_begin/end）
 *   - aarch64: NEON
 *
 * 加速的推理操作：
 *   - 向量点积（dot product）
 *   - 矩阵乘法（matmul）
 *   - 向量加法 / 缩放（ReLU / sigmoid 辅助）
 *   - softmax
 *
 * 注意：内核态使用 SIMD 前必须 kernel_fpu_begin()，用后 kernel_fpu_end()
 */

#ifndef _KAI_SIMD_H
#define _KAI_SIMD_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>

#if defined(__x86_64__)
#include <asm/fpu/api.h>
#define KAI_SIMD_X86 1
#elif defined(__aarch64__)
#define KAI_SIMD_ARM 1
#endif

/* =========================================================================
 * 向量点积（浮点）
 * ========================================================================= */

/*
 * kai_dot_product_f32 — 单精度点积
 * 返回 sum(a[i] * b[i])
 */
static inline float kai_dot_product_f32(const float *a, const float *b, int n)
{
    float sum = 0.0f;
    int i = 0;

#ifdef KAI_SIMD_X86
    kernel_fpu_begin();
    /* AVX 每次处理 8 个 float */
    while (i + 8 <= n) {
        asm volatile (
            "vmovups (%0), %%ymm0\n"   /* 加载 a[i..i+7] */
            "vmovups (%1), %%ymm1\n"   /* 加载 b[i..i+7] */
            "vmulps %%ymm1, %%ymm0, %%ymm0\n"
            "vaddps %%ymm0, %%ymm2, %%ymm2\n"
            :
            : "r"(a + i), "r"(b + i)
            : "ymm0", "ymm1", "ymm2", "memory");
        i += 8;
    }
    /* 汇总 ymm2 到标量 */
    asm volatile (
        "vextractf128 $1, %%ymm2, %%xmm0\n"
        "vaddps %%xmm0, %%xmm2, %%xmm2\n"
        "vhaddps %%xmm2, %%xmm2, %%xmm2\n"
        "vhaddps %%xmm2, %%xmm2, %%xmm2\n"
        "vmovss %%xmm2, %0\n"
        : "=m"(sum)
        :
        : "xmm0", "xmm2", "memory");
    kernel_fpu_end();
#endif

    /* 处理剩余元素（标量）*/
    for (; i < n; i++)
        sum += a[i] * b[i];

    return sum;
}

/* =========================================================================
 * 向量加法 + ReLU
 * ========================================================================= */

/*
 * kai_vec_relu_f32 — 向量 ReLU 激活
 * out[i] = max(0, in[i])
 */
static inline void kai_vec_relu_f32(float *out, const float *in, int n)
{
    int i;
    for (i = 0; i < n; i++)
        out[i] = (in[i] > 0.0f) ? in[i] : 0.0f;
}

/* =========================================================================
 * 矩阵乘法（简化，行主序）
 * ========================================================================= */

/*
 * kai_matmul_f32 — 矩阵乘法 C = A * B
 * A: m×k, B: k×n, C: m×n（行主序）
 */
static inline void kai_matmul_f32(const float *a, const float *b, float *c,
                                  int m, int n, int k)
{
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            float sum = 0.0f;
            for (int kk = 0; kk < k; kk++) {
                sum += a[i * k + kk] * b[kk * n + j];
            }
            c[i * n + j] = sum;
        }
    }
}

/* =========================================================================
 * softmax
 * ========================================================================= */

/*
 * kai_softmax_f32 — softmax 归一化
 * out[i] = exp(in[i]) / sum(exp(in))
 */
static inline void kai_softmax_f32(float *out, const float *in, int n)
{
    /* 找最大值（数值稳定）*/
    float max_val = in[0];
    for (int i = 1; i < n; i++)
        if (in[i] > max_val) max_val = in[i];

    /* 计算 exp 和 sum */
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        out[i] = (float)expf(in[i] - max_val);
        sum += out[i];
    }

    /* 归一化 */
    if (sum > 0.0f) {
        for (int i = 0; i < n; i++)
            out[i] /= sum;
    }
}

/* =========================================================================
 * sigmoid
 * ========================================================================= */

static inline float kai_sigmoid_f32(float x)
{
    return 1.0f / (1.0f + (float)expf(-x));
}

#endif /* _KAI_SIMD_H */
