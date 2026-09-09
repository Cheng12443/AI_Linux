/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ai_accel.h — 统一 AI 加速器抽象层（ROADMAP 7.1）
 *
 * 提供一套统一的硬件加速接口，屏蔽厂商差异：
 *   - NVIDIA: CUDA / cuDNN
 *   - AMD: ROCm / MIOpen
 *   - Intel: OpenVINO / Gaudi
 *   - 华为昇腾: CANN / ACL
 *   - 昆仑芯: Paddle Lite
 *   - RISC-V NPU: 自定义扩展指令
 *
 * 设计：
 *   1. 上层（ai_core/kai_infer）只依赖本抽象层
 *   2. 各厂商实现 AI_ACCEL_OPS 接口
 *   3. 运行时动态探测可用硬件
 */

#ifndef _AI_ACCEL_H
#define _AI_ACCEL_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>

/* =========================================================================
 * 厂商 / 设备类型
 * ========================================================================= */

enum accel_vendor {
    ACCEL_VENDOR_NONE      = 0,
    ACCEL_VENDOR_NVIDIA    = 1,  /* CUDA */
    ACCEL_VENDOR_AMD       = 2,  /* ROCm */
    ACCEL_VENDOR_INTEL     = 3,  /* OpenVINO / Gaudi */
    ACCEL_VENDOR_HUAWEI    = 4,  /* Ascend CANN */
    ACCEL_VENDOR_KUNLUN    = 5,  /* 昆仑芯 */
    ACCEL_VENDOR_RISCV     = 6,  /* RISC-V NPU */
    ACCEL_VENDOR_QUALCOMM  = 7,  /* Hexagon */
    ACCEL_VENDOR_CUSTOM    = 99,
};

enum accel_type {
    ACCEL_TYPE_GPU    = 0,  /* 通用 GPU */
    ACCEL_TYPE_NPU    = 1,  /* 神经网络处理器 */
    ACCEL_TYPE_TPU    = 2,  /* 张量处理器 */
    ACCEL_TYPE_FPGA   = 3,  /* FPGA */
    ACCEL_TYPE_CPU    = 4,  /* CPU SIMD 加速 */
};

/* =========================================================================
 * 设备信息
 * ========================================================================= */

struct accel_device_info {
    char         name[64];           /* 如 "NVIDIA A100" */
    char         driver_version[32];
    enum accel_vendor vendor;
    enum accel_type  type;
    __u64        memory_total;       /* 显存/内存字节 */
    __u64        memory_free;
    __u32        compute_units;      /* CUDA cores / NPU cores */
    __u32        max_threads;        /* 最大线程/线程数 */
    __u32        clock_mhz;
    char         pci_addr[32];       /* PCI 地址 */
    __u32        numa_node;
    __u32        status;             /* 0=offline, 1=online, 2=busy */
};

/* =========================================================================
 * 张量描述
 * ========================================================================= */

enum dtype {
    DTYPE_FP32 = 0,
    DTYPE_FP16 = 1,
    DTYPE_INT8 = 2,
    DTYPE_INT4 = 3,
    DTYPE_BF16 = 4,
};

struct accel_tensor {
    void        *data;          /* 数据指针 */
    __u32       *dims;          /* 维度数组 */
    __u32        ndim;
    enum dtype    dtype;
    __u64        size_bytes;
    __u32        flags;         /* 0=host, 1=device */
};

/* 张量布局（用于矩阵乘法）*/
struct matmul_params {
    __u32  M, N, K;
    __u32  transpose_a, transpose_b;
    enum dtype dtype;
    float  alpha, beta;
    void  *A, *B, *C;
    __u64  a_size, b_size, c_size;
};

/* =========================================================================
 * 推理请求（硬件执行）
 * ========================================================================= */

struct accel_infer_request {
    /* 模型 */
    void       *model;            /* 已加载模型的句柄 */
    char        model_name[64];

    /* 输入输出 */
    void       *input;
    __u64       input_size;
    void       *output;
    __u64       output_size;

    /* 执行参数 */
    __u32       priority;
    __u32       flags;            /* 0=sync, 1=async */

    /* 结果 */
    __s64       latency_ns;
    __s32       err;
};

/* =========================================================================
 * 硬件操作接口（各厂商实现）
 * ========================================================================= */

struct accel_ops {
    /* 探测设备 */
    int  (*probe)(struct accel_device_info *info, __u32 *count);

    /* 初始化 / 关闭 */
    int  (*init)(void);
    void (*fini)(void);

    /* 内存管理 */
    void *(*alloc_mem)(__u64 size, __u32 flags);
    int   (*free_mem)(void *ptr);
    int   (*copy_host_to_dev)(void *dst, const void *src, __u64 size);
    int   (*copy_dev_to_host)(void *dst, const void *src, __u64 size);

    /* 模型管理 */
    void *(*load_model)(const void *weights, __u64 size,
                        const char *name);
    int   (*unload_model)(void *model);

    /* 推理 */
    int  (*infer)(struct accel_infer_request *req);

    /* 张量运算 */
    int  (*matmul)(struct matmul_params *params);
    int  (*conv2d)(void *input, void *weights, void *output,
                   __u32 n, __u32 c, __u32 h, __u32 w,
                   __u32 out_c, __u32 kernel, __u32 stride);
    int  (*relu)(void *data, __u64 size);
    int  (*softmax)(void *data, __u64 size, __u32 axis);
    int  (*pooling)(void *input, void *output, __u32 type);

    /* 状态 */
    int  (*get_info)(struct accel_device_info *info);
    int  (*get_utilization)(__u32 *percent);
    __u64 (*get_memory_used)(void);
};

/* =========================================================================
 * 加速器设备
 * ========================================================================= */

struct accel_device {
    struct accel_device_info info;
    const struct accel_ops *ops;
    void                 *priv;       /* 厂商私有数据 */
    struct list_head     list;
    atomic_t             refcount;
};

/* =========================================================================
 * 公共 API
 * ========================================================================= */

/* 注册 / 注销加速器 */
int  ai_accel_register(struct accel_device *dev);
int  ai_accel_unregister(const char *name);

/* 探测所有硬件 */
int  ai_accel_probe_all(void);

/* 查找加速器 */
struct accel_device *ai_accel_get(const char *name);
struct accel_device *ai_accel_get_best(enum accel_vendor pref_vendor,
                                        __u32 min_memory_mb);
void ai_accel_put(struct accel_device *dev);

/* 推理（自动选择后端）*/
int  ai_accel_infer(struct accel_infer_request *req,
                    const char *pref_vendor);

/* 列出所有设备 */
int  ai_accel_list(struct accel_device_info *out, __u32 max_devices);

/* 统计 */
void ai_accel_stats(__u32 *online, __u32 *total,
                    __u64 *total_memory, __u64 *used_memory);

#endif /* _AI_ACCEL_H */
