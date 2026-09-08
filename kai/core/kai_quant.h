/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kai_quant.h — 模型量化支持
 *
 * 量化类型：
 *   - FP32 → FP16: 精度损失小，内存减半
 *   - FP32 → INT8: 精度损失可接受，内存 1/4
 *   - FP32 → INT4: 激进量化，内存 1/8
 */

#ifndef _KAI_QUANT_H
#define _KAI_QUANT_H

#include <linux/types.h>
#include <linux/math64.h>
#include <linux/string.h>

/* 量化类型 */
#define QUANT_FP32  0
#define QUANT_FP16  1
#define QUANT_INT8  2
#define QUANT_INT4  3

/* FP16 → FP32 */
static inline float fp16_to_fp32(__u16 h)
{
    union { __u32 u; float f; } conv;
    __u32 sign = (h & 0x8000) << 16;
    __u32 exponent = ((h >> 10) & 0x1F);
    __u32 mantissa = (h & 0x3FF);

    if (exponent == 0) {
        /* Denormalized */
        conv.u = sign | mantissa;
        conv.f = conv.f * powf(2.0f, -24.0f);
    } else if (exponent == 31) {
        /* Inf/NaN */
        conv.u = sign | 0x7F800000 | (mantissa << 13);
    } else {
        conv.u = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    return conv.f;
}

/* FP32 → FP16 */
static inline __u16 fp32_to_fp16(float f)
{
    union { float f; __u32 u; } conv = { .f = f };
    __u32 sign = (conv.u >> 16) & 0x8000;
    __s32 exponent = ((conv.u >> 23) & 0xFF) - 127;
    __u32 mantissa = conv.u & 0x7FFFFF;

    if (exponent < -14) {
        /* Denormalized FP16 */
        return sign;
    } else if (exponent > 15) {
        /* Overflow */
        return sign | 0x7C00;
    } else {
        return sign | ((exponent + 15) << 10) | (mantissa >> 13);
    }
}

/* INT8 量化/反量化 */
static inline __s8 float_to_int8(float f, float scale, __s32 zero_point)
{
    int val = (int)(f / scale) + zero_point;
    if (val > 127) val = 127;
    if (val < -128) val = -128;
    return (__s8)val;
}

static inline float int8_to_float(__s8 val, float scale, __s32 zero_point)
{
    return (val - zero_point) * scale;
}

/* INT4 量化（打包）*/
static inline __u8 float_to_int4(float f, float scale)
{
    int val = (int)(f / scale);
    if (val > 7) val = 7;
    if (val < -8) val = -8;
    return (__u8)(val & 0x0F);
}

static inline float int4_to_float(__u8 val, float scale)
{
    int v = (val & 0x08) ? (val | 0xF0) : val; /* 符号扩展 */
    return (float)((__s8)v) * scale;
}

/* 量化矩阵乘法（INT8）*/
static inline void
quant_matmul_int8(const __s8 *a, const __s8 *b, __s32 *c,
                  int m, int n, int k,
                  float scale_a, float scale_b, float scale_out)
{
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            __s32 sum = 0;
            for (int kk = 0; kk < k; kk++) {
                sum += a[i * k + kk] * b[kk * n + j];
            }
            c[i * n + j] = (__s32)(sum * scale_a * scale_b / scale_out);
        }
    }
}

/* 量化矩阵乘法（FP16）*/
static inline void
quant_matmul_fp16(const __u16 *a, const __u16 *b, __u16 *c,
                  int m, int n, int k)
{
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            float sum = 0.0f;
            for (int kk = 0; kk < k; kk++) {
                sum += fp16_to_fp32(a[i * k + kk]) * fp16_to_fp32(b[kk * n + j]);
            }
            c[i * n + j] = fp32_to_fp16(sum);
        }
    }
}

#endif /* _KAI_QUANT_H */
