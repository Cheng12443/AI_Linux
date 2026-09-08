// SPDX-License-Identifier: GPL-2.0
/*
 * ai_userland.c — 用户态 AI 推理服务守护进程
 *
 * 负责：
 *   - 从内核接收推理请求（via io_uring / netlink）
 *   - 加载和运行 ML 模型（ONNX / TFLite / PyTorch）
 *   - 返回推理结果给内核（via ring buffer / io_uring）
 *   - 管理模型生命周期
 *
 * 部署方式：
 *   ./ai_userlandd --model-dir /var/lib/ai/models \
 *                   --backend onnx \
 *                   --port 9999
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <liburing.h>
#include <onnxruntime_c_api.h>

#define DRV_NAME  "ai_userland"
#define MAX_MODELS 16
#define INFERENCE_TIMEOUT_MS 100

/* =========================================================================
 * 推理请求结构（与内核 ai_inference_request 对齐）
 * ========================================================================= */

struct inference_request {
    uint64_t id;
    char     model_name[64];
    void    *input;       /* mmap 共享内存 */
    size_t   input_size;
    void    *output;
    size_t   output_size;
    uint64_t timestamp_ns;
    int      mode;         /* 0=sync, 1=async */
};

struct inference_result {
    uint64_t id;
    int      err;
    float    scores[4];    /* 输出向量 */
    int      top_class;
    uint64_t latency_ns;
};

/* =========================================================================
 * 模型管理
 * ========================================================================= */

static struct {
    char     name[64];
    char     path[256];
    void    *ort_session;
    int      input_dim;
    int      output_dim;
    int      loaded;
} models[MAX_MODELS];

static int num_models = 0;

static void *load_onnx_model(const char *path)
{
    /* ONNX Runtime 初始化 */
    const OrtApi *api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    OrtEnv *env;
    OrtSessionOptions *sess_opts;
    OrtSession *session;

    OrtStatus *status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ai_userland", &env);
    if (status) {
        fprintf(stderr, "ORT CreateEnv failed\n");
        return NULL;
    }

    status = api->CreateSessionOptions(&sess_opts);
    if (status) {
        fprintf(stderr, "ORT CreateSessionOptions failed\n");
        return NULL;
    }
    api->SetSessionGraphOptimizationLevel(sess_opts, ORT_ENABLE_BASIC);

    status = api->CreateSession(env, path, sess_opts, &session);
    if (status) {
        fprintf(stderr, "ORT CreateSession failed: %s\n", path);
        return NULL;
    }

    printf("[ai_userland] Loaded model: %s\n", path);
    return session;
}

static int run_inference_onnx(void *session,
                               const float *input, int input_dim,
                               float *output, int output_dim)
{
    const OrtApi *api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    OrtMemoryInfo *mem_info;
    OrtValue *input_tensor = NULL;
    OrtValue *output_tensor = NULL;
    int64_t input_shape[2] = { 1, input_dim };
    int64_t output_shape[2] = { 1, output_dim };
    OrtStatus *status;

    api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);

    /* 创建输入 tensor */
    status = api->CreateTensorWithDataAsOrtValue(mem_info,
        (void *)input, input_dim * sizeof(float),
        input_shape, 2,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &input_tensor);
    if (status) { printf("Create input tensor failed\n"); return -1; }

    /* 运行推理 */
    const char *input_names[] = { "input" };
    const char *output_names[] = { "output" };

    status = api->Run(session, NULL,
                      input_names, (const OrtValue *const *)&input_tensor, 1,
                      output_names, 1, &output_tensor);
    if (status) { printf("Run failed\n"); return -1; }

    /* 读取输出 */
    float *out_ptr;
    api->GetTensorDataAsFloat(output_tensor, (void **)&out_ptr,
                              output_shape, 2);
    memcpy(output, out_ptr, output_dim * sizeof(float));

    api->ReleaseValue(input_tensor);
    api->ReleaseValue(output_tensor);
    api->ReleaseMemoryInfo(mem_info);

    return 0;
}

/* =========================================================================
 * 最简软推理（无 ONNX 依赖）—— 演示用
 * ========================================================================= */

static int soft_inference(const float *input, int input_dim,
                          float *output, int output_dim)
{
    /* 演示：线性加权评分模型
     * output[0] = sigmoid(sum(input[i] * w[i]) + b)
     * 演示权重（全1）*/
    float sum = 0.0f;
    for (int i = 0; i < input_dim; i++)
        sum += input[i];

    float score = 1.0f / (1.0f + expf(-(sum / input_dim - 0.5f) * 10.0f));

    output[0] = score;
    output[1] = 1.0f - score;
    output[2] = 0.0f;
    output[3] = 0.0f;

    return 0;
}

/* =========================================================================
 * io_uring 异步推理队列
 * ========================================================================= */

static struct io_uring ring;

static void setup_io_uring(void)
{
    int ret = io_uring_queue_init(128, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring init failed: %d\n", ret);
        exit(1);
    }
    printf("[ai_userland] io_uring ready (queue depth=128)\n");
}

static void inference_loop(void)
{
    struct io_uring_cqe *cqe;
    unsigned head;

    printf("[ai_userland] Inference loop started\n");

    while (1) {
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            fprintf(stderr, "wait_cqe error: %d\n", ret);
            break;
        }

        /* 处理完成事件 */
        struct inference_request *req = (void *)(uintptr_t)cqe->user_data;
        if (req && cqe->res >= 0) {
            /* 推理执行 */
            float scores[4] = { 0 };
            uint64_t start = __builtin_bswap64(req->timestamp_ns);
            uint64_t now;
            int err = 0;

            /* 软推理（演示） */
            err = soft_inference(req->input, req->input_size / sizeof(float),
                                 scores, 4);

            now = __builtin_bswap64(req->timestamp_ns);

            /* 写回结果 */
            struct inference_result result = {
                .id        = req->id,
                .err       = err,
                .scores    = { scores[0], scores[1], scores[2], scores[3] },
                .top_class = scores[0] > scores[1] ? 0 : 1,
                .latency_ns = now - start,
            };
            /* 结果通过共享内存或 netlink 返回 */

            if (err == 0) {
                printf("[ai] infer id=%lu score=%.4f latency=%luns\n",
                       req->id, scores[0], result.latency_ns);
            }
        }

        io_uring_cqe_seen(&ring, cqe);
    }
}

/* =========================================================================
 * 信号处理
 * ========================================================================= */

static volatile int running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* =========================================================================
 * 主函数
 * ========================================================================= */

static void print_banner(void)
{
    printf("\n");
    printf("  ╔═══════════════════════════════════════════╗\n");
    printf("  ║   AI Linux — User-Space Inference Daemon ║\n");
    printf("  ║   v1.0.0                                  ║\n");
    printf("  ╚═══════════════════════════════════════════╝\n");
    printf("\n");
}

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --model-dir DIR    模型目录 (default: /var/lib/ai/models)\n");
    printf("  --backend NAME     推理后端: onnx|soft (default: soft)\n");
    printf("  --help             显示帮助\n");
}

int main(int argc, char **argv)
{
    const char *model_dir = "/var/lib/ai/models";
    const char *backend   = "soft";
    int ch;

    print_banner();

    while ((ch = getopt(argc, argv, "m:b:h")) != -1) {
        switch (ch) {
        case 'm': model_dir = optarg; break;
        case 'b': backend   = optarg; break;
        case 'h': usage(argv[0]); return 0;
        }
    }

    printf("[ai_userland] Backend: %s\n", backend);
    printf("[ai_userland] Model dir: %s\n", model_dir);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* 初始化推理引擎 */
    if (strcmp(backend, "onnx") == 0) {
        printf("[ai_userland] ONNX Runtime backend (placeholder — link libonnxruntime to enable)\n");
    } else {
        printf("[ai_userland] Soft inference backend ready\n");
    }

    /* 初始化 io_uring */
    setup_io_uring();

    /* 启动推理循环 */
    inference_loop();

    io_uring_queue_exit(&ring);
    printf("[ai_userland] Exiting\n");
    return 0;
}
