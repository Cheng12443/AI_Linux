// SPDX-License-Identifier: GPL-2.0
/*
 * ai_layer2_gateway.c — Layer2 AI 网关（仅 DeepSeek + Kimi K3）
 *
 * 两个后端：
 *   DeepSeek  — https://api.deepseek.com
 *   Kimi K3   — https://api.moonshot.cn
 *
 * 架构：
 *   内核 (Layer1) ←netlink→ 网关 ←HTTP→ DeepSeek / Kimi K3
 *
 * 编译（无依赖版）：
 *   gcc -O2 -o ai_layer2_gateway ai_layer2_gateway.c -lpthread
 *
 * 完整编译：
 *   gcc -O2 -o ai_layer2_gateway ai_layer2_gateway.c \
 *       -lcurl -ljson-c -lpthread -lssl -lcrypto
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <ctype.h>
#include <stdarg.h>

/* =========================================================================
 * 常量与版本
 * ========================================================================= */

#define VERSION          "1.0.0"
#define BUILD_DATE       __DATE__ " " __TIME__

/* =========================================================================
 * DeepSeek + Kimi K3 API 配置
 * ========================================================================= */

/* DeepSeek */
#define DEEPSEEK_HOST    "api.deepseek.com"
#define DEEPSEEK_PORT    443
#define DEEPSEEK_PATH    "/v1/chat/completions"
#define DEEPSEEK_MODEL  "deepseek-chat"

static const char *deepseek_models[] = {
    "deepseek-chat",
    "deepseek-coder",
    "deepseek-reasoner",
};

/* Kimi K3 (Moonshot AI) */
#define KIMI_HOST        "api.moonshot.cn"
#define KIMI_PORT        443
#define KIMI_PATH        "/v1/chat/completions"
#define KIMI_MODEL       "moonshot-v1-8k"

static const char *kimi_models[] = {
    "moonshot-v1-8k",
    "moonshot-v1-32k",
    "moonshot-v1-128k",
};

/* 最大值 */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* =========================================================================
 * 日志
 * ========================================================================= */

static int log_level = 1; /* 0=silent, 1=info, 2=debug */
static FILE *log_fp = NULL;

static void log_set_level(int lvl) { log_level = lvl; }
static void log_set_file(FILE *fp) { log_fp = fp; }

static void _log(int lvl, const char *fmt, ...)
{
    if (lvl > log_level) return;

    FILE *out = log_fp ? log_fp : stdout;
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));

    const char *prefix = (lvl == 0) ? "ERROR" :
                         (lvl == 1) ? "INFO " : "DEBUG";

    fprintf(out, "[%s] [%s] ", ts, prefix);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);
}

#define LOGE(fmt, ...)  _log(0, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...)  _log(1, fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...)  _log(2, fmt, ##__VA_ARGS__)

/* =========================================================================
 * 时间
 * ========================================================================= */

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static unsigned long long now_ms(void)
{
    return now_ns() / 1000000ULL;
}

/* =========================================================================
 * 字符串工具
 * ========================================================================= */

static int str_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void str_trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
}

static char *str_strnchr(char *s, int c, size_t maxlen)
{
    size_t i = 0;
    while (i < maxlen && s[i]) {
        if (s[i] == c) return &s[i];
        i++;
    }
    return NULL;
}

/* 简单的 URL 编码（用于 JSON 转义） */
static void json_escape(const char *in, char *out, size_t out_size)
{
    const char *p = in;
    char *op = out;
    size_t left = out_size - 1;

    while (*p && left > 4) {
        switch (*p) {
        case '"':  strcpy(op, "\\\""); op += 2; left -= 2; break;
        case '\\': strcpy(op, "\\\\"); op += 2; left -= 2; break;
        case '\n': strcpy(op, "\\n");  op += 2; left -= 2; break;
        case '\r': strcpy(op, "\\r");  op += 2; left -= 2; break;
        case '\t': strcpy(op, "\\t");  op += 2; left -= 2; break;
        default:
            if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f) {
                *op++ = *p; left--;
            } else {
                snprintf(op, left, "\\u%04x", (unsigned char)*p);
                op += 6; left -= 6;
            }
        }
        p++;
    }
    *op = '\0';
}

/* =========================================================================
 * 内存分配
 * ========================================================================= */

static void *xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) { LOGE("OOM"); exit(1); }
    return p;
}

static void *xcalloc(size_t nmemb, size_t size)
{
    void *p = calloc(nmemb, size);
    if (!p) { LOGE("OOM"); exit(1); }
    return p;
}

static void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (!p && size) { LOGE("OOM"); exit(1); }
    return p;
}

static char *xstrdup(const char *s)
{
    return strcpy(xmalloc(strlen(s) + 1), s);
}

/* =========================================================================
 * 配置（全局）
 * ========================================================================= */

typedef enum {
    BACKEND_DEEPSEEK = 0,
    BACKEND_KIMI     = 1,
} Backend;

typedef enum {
    MODE_DISABLED  = 0,
    MODE_ASYNC     = 1,   /* Layer1 先决策，Layer2 异步复核（推荐） */
    MODE_SYNC      = 2,   /* Layer1 等待 Layer2 */
    MODE_OVERRIDE  = 3,   /* Layer2 覆盖 Layer1 */
} RouterMode;

typedef struct {
    /* 后端 */
    Backend  backend;           /* 0=DeepSeek, 1=Kimi */
    char     api_key[256];      /* API 密钥 */

    /* 模型 */
    char     model[64];         /* 当前模型 */

    /* 运行模式 */
    RouterMode mode;

    /* 阈值 */
    int       sched_threshold;  /* 调度置信度阈值 (0-10000) */
    int       io_threshold;
    int       sec_threshold;

    /* 降级 */
    int       sync_timeout_ms;
    int       api_timeout_ms;
    int       max_retries;

    /* 统计 */
    int       debug;
} Config;

static Config cfg = {
    .backend          = BACKEND_DEEPSEEK,
    .api_key          = { 0 },
    .model            = DEEPSEEK_MODEL,
    .mode             = MODE_ASYNC,
    .sched_threshold  = 7000,
    .io_threshold     = 7000,
    .sec_threshold   = 8000,
    .sync_timeout_ms  = 1000,
    .api_timeout_ms   = 5000,
    .max_retries     = 3,
    .debug            = 0,
};

/* =========================================================================
 * 统计
 * ========================================================================= */

typedef struct {
    unsigned long long decisions_sent;
    unsigned long long decisions_received;
    unsigned long long api_calls;
    unsigned long long api_success;
    unsigned long long api_errors;
    unsigned long long api_timeout;
    unsigned long long deepseek_calls;
    unsigned long long kimi_calls;
    unsigned long long nl_sent;
    unsigned long long nl_received;
    unsigned long long nl_errors;
    unsigned long long override_layer1;
    unsigned long long cache_hits;
    unsigned long long cache_misses;
    unsigned long long errors;
    unsigned long long start_time_ms;
} Stats;

static Stats stats = { 0 };

/* =========================================================================
 * 决策缓存
 * ========================================================================= */

#define CACHE_SIZE 4096

typedef struct {
    unsigned long long key;
    unsigned char      decision;
    unsigned char      confidence;
    unsigned char      domain;
    unsigned long long expires_ms;
    unsigned char      used; /* 0=free */
} CacheEntry;

static CacheEntry cache[CACHE_SIZE];
static int cache_idx = 0;

static unsigned long long cache_make_key(const void *data, size_t len)
{
    unsigned long long h = 5381;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len && i < 128; i++)
        h = ((h << 5) + h) + p[i];
    return h;
}

static int cache_get(unsigned long long key, unsigned long long now_ms,
                     unsigned char *decision, unsigned char *confidence)
{
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].used && cache[i].key == key && cache[i].expires_ms > now_ms) {
            *decision = cache[i].decision;
            *confidence = cache[i].confidence;
            return 0;
        }
    }
    return -1;
}

static void cache_put(unsigned long long key, unsigned char decision,
                     unsigned char confidence, unsigned char domain)
{
    int idx = (cache_idx++) % CACHE_SIZE;
    cache[idx % CACHE_SIZE] = (CacheEntry){
        .key         = key,
        .decision     = decision,
        .confidence   = confidence,
        .domain       = domain,
        .expires_ms   = now_ms() + 5000,
        .used         = 1,
    };
}

/* =========================================================================
 * 提示词工程
 * ========================================================================= */

/* 域标识 */
#define DOMAIN_SCHED   0
#define DOMAIN_IO      1
#define DOMAIN_SEC     2
#define DOMAIN_MEM     3

/* 调度决策值 */
#define DEC_KEEP    0
#define DEC_PROMOTE 1
#define DEC_DEMOTE  2
#define DEC_MIGRATE 3
#define DEC_BATCH   4
#define DEC_IDLE    5

/* IO 决策值（映射）*/
#define DEC_IOPASS     100
#define DEC_IODROP     101
#define DEC_IOREDIRECT 102
#define DEC_IOALERT    103

/* 安全决策值 */
#define DEC_SEC_ALLOW  0
#define DEC_SEC_BLOCK  1
#define DEC_SEC_ALERT  2

static const char *decision_str_sched(int d)
{
    switch (d) {
    case DEC_PROMOTE: return "promote";
    case DEC_DEMOTE:  return "demote";
    case DEC_MIGRATE: return "migrate";
    case DEC_BATCH:   return "batch";
    case DEC_IDLE:    return "idle";
    default:          return "keep";
    }
}

static const char *decision_str_io(int d)
{
    switch (d) {
    case DEC_IODROP:     return "drop";
    case DEC_IOREDIRECT: return "redirect";
    case DEC_IOALERT:    return "alert";
    default:             return "pass";
    }
}

static const char *decision_str_sec(int d)
{
    switch (d) {
    case DEC_SEC_BLOCK: return "block";
    case DEC_SEC_ALERT: return "alert";
    default:            return "allow";
    }
}

/*
 * 构建提示词
 * 输入特征以十六进制形式注入（避免特殊字符问题）
 */
static void build_prompt(char *buf, size_t bufsize,
                        int domain,
                        const unsigned char *feat, size_t feat_len,
                        unsigned char layer1_decision, int layer1_confidence)
{
    char feat_hex[512] = { 0 };
    for (size_t i = 0; i < feat_len && i < 128; i++)
        snprintf(feat_hex + strlen(feat_hex), sizeof(feat_hex) - strlen(feat_hex),
                 "%02x", feat[i]);

    if (domain == DOMAIN_SCHED) {
        snprintf(buf, bufsize,
            "你是一个运行在 Linux 内核中的 AI 调度助手。\n\n"
            "请根据以下任务特征和系统状态，给出最优的 CPU 调度决策。\n\n"
            "任务特征（十六进制）：%s\n"
            "Layer1 内核 AI 当前决策：%s（置信度 %d%%）\n\n"
            "决策选项（只能选一个）：\n"
            "  - promote : 升权，给任务更多 CPU 时间片\n"
            "  - demote  : 降权，减少 CPU 时间片\n"
            "  - migrate : 迁移到其他 CPU\n"
            "  - batch   : 标记为批处理任务\n"
            "  - idle    : 插入 idle（暂停）\n"
            "  - keep    : 保持现状\n\n"
            "请只返回以下 JSON 格式，不要包含任何其他文字：\n"
            "{\"decision\":\"选项\",\"confidence\":0-100,\"reason\":\"中文理由\"}\n",
            feat_hex,
            decision_str_sched(layer1_decision), layer1_confidence);

    } else if (domain == DOMAIN_IO) {
        snprintf(buf, bufsize,
            "你是一个运行在 Linux 内核中的 AI 网络安全分析助手。\n\n"
            "请分析以下网络流量特征，判断是否放行。\n\n"
            "流量特征（十六进制）：%s\n\n"
            "决策选项（只能选一个）：\n"
            "  - drop     : 丢弃（拒绝）\n"
            "  - redirect : 重定向到慢路径\n"
            "  - alert    : 记录告警\n"
            "  - pass     : 正常放行\n\n"
            "请只返回以下 JSON 格式：\n"
            "{\"decision\":\"选项\",\"confidence\":0-100,\"reason\":\"中文理由\"}\n",
            feat_hex);

    } else if (domain == DOMAIN_SEC) {
        snprintf(buf, bufsize,
            "你是一个运行在 Linux 内核中的 AI 安全检测助手。\n\n"
            "请分析以下安全事件，判断是否阻止。\n\n"
            "事件特征（十六进制）：%s\n\n"
            "决策选项（只能选一个）：\n"
            "  - block : 阻止执行\n"
            "  - alert : 记录告警但不阻止\n"
            "  - allow : 允许\n\n"
            "请只返回以下 JSON 格式：\n"
            "{\"decision\":\"选项\",\"confidence\":0-100,\"reason\":\"中文理由\"}\n",
            feat_hex);
    } else {
        snprintf(buf, bufsize,
            "{\"decision\":\"keep\",\"confidence\":50,\"reason\":\"未知域\"}");
    }
}

/* =========================================================================
 * JSON 解析（无依赖，最简实现）
 * ========================================================================= */

static int json_get_int(const char *json, const char *key)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == '"') p++;
    int val = atoi(p);
    return val;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == '"') p++;
    const char *end = strchr(p, '"');
    if (!end) end = p + strlen(p);
    size_t len = end - p;
    if (len > out_size - 1) len = out_size - 1;
    strncpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* 从响应体中提取 content */
static int extract_content(const char *http_body, char *out, size_t out_size)
{
    /* 找 "choices"[0]."message"."content" */
    const char *p = http_body;

    /* 找 choices 数组 */
    p = strstr(p, "\"choices\"");
    if (!p) return -1;
    p = strchr(p, '[');
    if (!p) return -1;
    p++; /* 跳过 [ */

    /* 找 message */
    p = strstr(p, "\"message\"");
    if (!p) return -1;
    p = strstr(p, "\"content\"");
    if (!p) return -1;
    p += 9; /* 跳过 "content": */

    while (*p == ' ' || *p == ':' || *p == '"') p++;

    const char *end = strchr(p, '"');
    if (!end) return -1;

    size_t len = end - p;
    if (len > out_size - 1) len = out_size - 1;
    strncpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* =========================================================================
 * HTTP 客户端（手写，不依赖 libcurl）
 * ========================================================================= */

typedef struct {
    char     host[256];
    int      port;
    char     path[256];
    char     api_key[256];
    int      timeout_ms;
} HttpRequest;

typedef struct {
    int      status;
    char     body[32768];
    size_t   body_len;
    unsigned long long latency_ns;
    char     error[256];
} HttpResponse;

static int tcp_connect(const char *host, int port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* 设置非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct hostent *he = gethostbyname(host);
    if (!he) { close(fd); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    /* 等待连接完成 */
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    ret = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 5000);
    if (ret <= 0) { close(fd); return -1; }

    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (err) { close(fd); return -1; }

    /* 恢复阻塞模式 */
    fcntl(fd, F_SETFL, flags);

    return fd;
}

static int tcp_send(int fd, const void *data, size_t len, int timeout_ms)
{
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int ret = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 3000);
        if (ret <= 0) return -1;

        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int tcp_recv(int fd, void *buf, size_t max_len, int timeout_ms)
{
    size_t total = 0;
    while (total < max_len - 1) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 10000);
        if (ret <= 0) break;

        ssize_t n = recv(fd, buf + total, max_len - total - 1, 0);
        if (n <= 0) break;
        total += n;
    }
    ((char *)buf)[total] = '\0';
    return total;
}

/* SSL 模拟（直接发送明文，生产环境应使用 OpenSSL）*/
static int https_request(const HttpRequest *req, HttpResponse *resp)
{
    memset(resp, 0, sizeof(*resp));

    unsigned long long start = now_ns();

    /* TCP 连接 */
    int fd = tcp_connect(req->host, req->port, req->timeout_ms);
    if (fd < 0) {
        snprintf(resp->error, sizeof(resp->error), "connect failed to %s:%d",
                 req->host, req->port);
        return -1;
    }

    /* 构建 HTTP 请求 */
    char req_buf[16384];
    int req_len = snprintf(req_buf, sizeof(req_buf),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        req->path, req->host, req->api_key, strlen(req->api_key) + 128);

    /* 发送请求头（实际请求体在外部追加，这里简化）*/
    (void)req_len;

    /* 关闭连接标记 */
    close(fd);
    resp->latency_ns = now_ns() - start;
    snprintf(resp->error, sizeof(resp->error), "no_ssl_support");
    return -1;
}

/*
 * 实际 HTTP 调用（构建完整请求）
 * 注意：这里使用简化实现，生产环境应使用 libcurl 或 OpenSSL
 */
static int do_http_post(const char *host, int port, const char *path,
                       const char *api_key,
                       const char *body, size_t body_len,
                       HttpResponse *resp,
                       int timeout_ms)
{
    memset(resp, 0, sizeof(*resp));

    unsigned long long start = now_ns();

    int fd = tcp_connect(host, port, timeout_ms > 0 ? timeout_ms : 5000);
    if (fd < 0) {
        resp->status = 0;
        snprintf(resp->error, sizeof(resp->error), "connection failed");
        return -1;
    }

    /* 构建 HTTP 请求 */
    char header[2048];
    int hdr_len = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Content-Length: %zu\r\n"
        "User-Agent: AI-Layer2-Gateway/1.0\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, api_key, body_len);

    /* 发送 header */
    if (tcp_send(fd, header, hdr_len, timeout_ms) < 0) {
        close(fd);
        resp->status = 0;
        snprintf(resp->error, sizeof(resp->error), "send header failed");
        return -1;
    }

    /* 发送 body */
    if (tcp_send(fd, body, body_len, timeout_ms) < 0) {
        close(fd);
        resp->status = 0;
        snprintf(resp->error, sizeof(resp->error), "send body failed");
        return -1;
    }

    /* 接收响应 */
    char raw_resp[65536];
    int recv_len = tcp_recv(fd, raw_resp, sizeof(raw_resp), timeout_ms + 2000);
    close(fd);

    resp->latency_ns = now_ns() - start;

    if (recv_len <= 0) {
        resp->status = 0;
        snprintf(resp->error, sizeof(resp->error), "recv failed or timeout");
        return -1;
    }

    /* 解析 HTTP 状态码 */
    char *body_start = strstr(raw_resp, "\r\n\r\n");
    if (!body_start) {
        snprintf(resp->error, sizeof(resp->error), "invalid http response");
        return -1;
    }
    *body_start = '\0';
    body_start += 4;

    /* 解析 status line */
    char *status_line = raw_resp;
    if (strncmp(status_line, "HTTP/1.1 ", 9) == 0)
        resp->status = atoi(status_line + 9);
    else if (strncmp(status_line, "HTTP/1.0 ", 9) == 0)
        resp->status = atoi(status_line + 9);
    else
        resp->status = 0;

    /* 提取 body（处理 chunked 编码）*/
    char *body_ptr = body_start;
    if (strstr(raw_resp, "Transfer-Encoding: chunked")) {
        /* 简单 chunked 解码 */
        char *out = resp->body;
        size_t out_left = sizeof(resp->body) - 1;
        char *p = body_ptr;
        while (p && *p && out_left > 0) {
            char *line_end = strchr(p, '\r');
            if (!line_end) break;
            *line_end = '\0';
            int chunk_len = strtol(p, NULL, 16);
            if (chunk_len == 0) break;
            p = line_end + 2; /* 跳过 \r\n */
            size_t copy_len = MIN(chunk_len, out_left - 1);
            memcpy(out, p, copy_len);
            out += copy_len;
            out_left -= copy_len;
            p += chunk_len + 2; /* 跳过 chunk + \r\n */
        }
        resp->body_len = out - resp->body;
        resp->body[resp->body_len] = '\0';
    } else {
        /* 找 Content-Length */
        const char *cl = strstr(raw_resp, "Content-Length:");
        if (cl) {
            cl += 15;
            while (*cl == ' ') cl++;
            int content_len = atoi(cl);
            if (content_len > (int)sizeof(resp->body) - 1)
                content_len = sizeof(resp->body) - 1;
            strncpy(resp->body, body_ptr, content_len);
            resp->body[content_len] = '\0';
            resp->body_len = content_len;
        } else {
            size_t copy_len = MIN((size_t)(recv_len - (body_ptr - raw_resp)),
                                 sizeof(resp->body) - 1);
            strncpy(resp->body, body_ptr, copy_len);
            resp->body[copy_len] = '\0';
            resp->body_len = copy_len;
        }
    }

    LOGD("HTTP %d %s (body=%zu bytes, latency=%llu ms)",
         resp->status, resp->error,
         resp->body_len, resp->latency_ns / 1000000ULL);

    return resp->status >= 200 && resp->status < 300 ? 0 : -1;
}

/* =========================================================================
 * DeepSeek API 调用
 * ========================================================================= */

static int call_deepseek(const char *prompt,
                        unsigned char *decision,
                        unsigned char *confidence,
                        char *reason, size_t reason_size,
                        unsigned long long *latency_ns)
{
    if (!cfg.api_key[0]) {
        LOGE("DeepSeek API key not set");
        return -1;
    }

    char body[8192];
    char escaped_prompt[4096];
    json_escape(prompt, escaped_prompt, sizeof(escaped_prompt));

    int body_len = snprintf(body, sizeof(body),
        "{"
        "\"model\":\"%s\","
        "\"messages\":["
        "  {\"role\":\"user\",\"content\":\"%s\"}"
        "],"
        "\"temperature\":0.1,"
        "\"max_tokens\":256"
        "}",
        cfg.model, escaped_prompt);

    HttpResponse resp;
    int ret = do_http_post(DEEPSEEK_HOST, DEEPSEEK_PORT, DEEPSEEK_PATH,
                           cfg.api_key, body, body_len,
                           &resp, cfg.api_timeout_ms);

    *latency_ns = resp.latency_ns;

    if (ret < 0) {
        LOGE("DeepSeek API failed: %s", resp.error);
        return -1;
    }

    if (resp.status != 200) {
        LOGE("DeepSeek API HTTP %d: %.100s", resp.status, resp.body);
        return -1;
    }

    /* 提取 AI 响应 */
    char ai_content[4096];
    if (extract_content(resp.body, ai_content, sizeof(ai_content)) < 0) {
        LOGE("DeepSeek: failed to extract content from response");
        return -1;
    }

    /* 解析 JSON */
    int conf = json_get_int(ai_content, "confidence");
    if (conf < 0) conf = 50;
    *confidence = (unsigned char)(conf > 100 ? 100 : conf);

    if (reason)
        json_get_string(ai_content, "reason", reason, reason_size);

    char dec_str[32];
    json_get_string(ai_content, "decision", dec_str, sizeof(dec_str));
    str_trim(dec_str);

    if (strcmp(dec_str, "promote") == 0) *decision = DEC_PROMOTE;
    else if (strcmp(dec_str, "demote") == 0)  *decision = DEC_DEMOTE;
    else if (strcmp(dec_str, "migrate") == 0) *decision = DEC_MIGRATE;
    else if (strcmp(dec_str, "batch") == 0)   *decision = DEC_BATCH;
    else if (strcmp(dec_str, "idle") == 0)    *decision = DEC_IDLE;
    else if (strcmp(dec_str, "drop") == 0)    *decision = DEC_IODROP;
    else if (strcmp(dec_str, "redirect") == 0)*decision = DEC_IOREDIRECT;
    else if (strcmp(dec_str, "alert") == 0)  *decision = DEC_IOALERT;
    else if (strcmp(dec_str, "block") == 0)  *decision = DEC_SEC_BLOCK;
    else *decision = DEC_KEEP;

    return 0;
}

/* =========================================================================
 * Kimi K3 API 调用
 * ========================================================================= */

static int call_kimi(const char *prompt,
                    unsigned char *decision,
                    unsigned char *confidence,
                    char *reason, size_t reason_size,
                    unsigned long long *latency_ns)
{
    if (!cfg.api_key[0]) {
        LOGE("Kimi K3 API key not set");
        return -1;
    }

    char body[8192];
    char escaped_prompt[4096];
    json_escape(prompt, escaped_prompt, sizeof(escaped_prompt));

    int body_len = snprintf(body, sizeof(body),
        "{"
        "\"model\":\"%s\","
        "\"messages\":["
        "  {\"role\":\"user\",\"content\":\"%s\"}"
        "],"
        "\"temperature\":0.1,"
        "\"max_tokens\":256"
        "}",
        cfg.model, escaped_prompt);

    HttpResponse resp;
    int ret = do_http_post(KIMI_HOST, KIMI_PORT, KIMI_PATH,
                           cfg.api_key, body, body_len,
                           &resp, cfg.api_timeout_ms);

    *latency_ns = resp.latency_ns;

    if (ret < 0) {
        LOGE("Kimi K3 API failed: %s", resp.error);
        return -1;
    }

    if (resp.status != 200) {
        LOGE("Kimi K3 API HTTP %d: %.100s", resp.status, resp.body);
        return -1;
    }

    char ai_content[4096];
    if (extract_content(resp.body, ai_content, sizeof(ai_content)) < 0) {
        LOGE("Kimi K3: failed to extract content");
        return -1;
    }

    int conf = json_get_int(ai_content, "confidence");
    if (conf < 0) conf = 50;
    *confidence = (unsigned char)(conf > 100 ? 100 : conf);

    if (reason)
        json_get_string(ai_content, "reason", reason, reason_size);

    char dec_str[32];
    json_get_string(ai_content, "decision", dec_str, sizeof(dec_str));
    str_trim(dec_str);

    if (strcmp(dec_str, "promote") == 0) *decision = DEC_PROMOTE;
    else if (strcmp(dec_str, "demote") == 0)  *decision = DEC_DEMOTE;
    else if (strcmp(dec_str, "migrate") == 0) *decision = DEC_MIGRATE;
    else if (strcmp(dec_str, "batch") == 0)   *decision = DEC_BATCH;
    else if (strcmp(dec_str, "idle") == 0)    *decision = DEC_IDLE;
    else if (strcmp(dec_str, "drop") == 0)    *decision = DEC_IODROP;
    else if (strcmp(dec_str, "redirect") == 0)*decision = DEC_IOREDIRECT;
    else if (strcmp(dec_str, "alert") == 0)  *decision = DEC_IOALERT;
    else if (strcmp(dec_str, "block") == 0)  *decision = DEC_SEC_BLOCK;
    else *decision = DEC_KEEP;

    return 0;
}

/* =========================================================================
 * 主推理入口
 * ========================================================================= */

static int do_inference(int domain,
                        const unsigned char *feat, size_t feat_len,
                        unsigned char layer1_dec, int layer1_conf,
                        unsigned char *decision,
                        unsigned char *confidence,
                        char *reason, size_t reason_size,
                        unsigned long long *latency_ns)
{
    /* 构建提示词 */
    char prompt[8192];
    build_prompt(prompt, sizeof(prompt), domain, feat, feat_len,
                 layer1_dec, layer1_conf);

    /* 检查缓存 */
    unsigned long long key = cache_make_key(feat, feat_len);
    unsigned long long now = now_ms();

    if (cache_get(key, now, decision, confidence) == 0) {
        stats.cache_hits++;
        if (reason) strncpy(reason, "cache_hit", reason_size - 1);
        *latency_ns = 0;
        LOGD("cache hit for domain=%d", domain);
        return 0;
    }
    stats.cache_misses++;

    int ret = -1;
    unsigned char dec = DEC_KEEP;
    unsigned char conf = 50;
    unsigned long long lat = 0;

    /* 调用 API */
    if (cfg.backend == BACKEND_DEEPSEEK) {
        stats.deepseek_calls++;
        ret = call_deepseek(prompt, &dec, &conf, reason, reason_size, &lat);
    } else {
        stats.kimi_calls++;
        ret = call_kimi(prompt, &dec, &conf, reason, reason_size, &lat);
    }

    if (ret == 0) {
        *decision = dec;
        *confidence = conf;
        *latency_ns = lat;

        /* 缓存结果 */
        cache_put(key, dec, conf, (unsigned char)domain);

        /* 统计 */
        stats.api_success++;
    } else {
        stats.api_errors++;
        *decision = layer1_dec;  /* 回退到 Layer1 */
        *confidence = (unsigned char)layer1_conf;
        if (reason) strncpy(reason, "api_failed_fallback_to_layer1", reason_size - 1);
        *latency_ns = lat;
    }

    stats.api_calls++;
    return ret;
}

/* =========================================================================
 * netlink（简化版，不依赖 libnl）
 * ========================================================================= */

/*
 * 我们不实现真正的 netlink 连接，
 * 而是模拟 netlink 接口。
 * 真实环境替换为标准 netlink 客户端。
 *
 * 以下是模拟层，用于在没有真实内核的情况下演示流程。
 */

#define NL_FD_MAX  8

typedef struct {
    int                fd;
    int                valid;
    pthread_t           thread;
    int                running;
    unsigned int        pid;
} NlClient;

static NlClient nl_client = { .fd = -1, .valid = 0 };

/*
 * 模拟：接收来自内核的决策请求
 * 真实实现：通过 netlink socket 接收
 */
static int nl_recv_request(int fd,
                           unsigned long long *request_id,
                           int *domain,
                           unsigned char *feat, size_t *feat_len,
                           unsigned char *layer1_dec, int *layer1_conf)
{
    /*
     * 模拟数据：当没有真实 netlink 时，
     * 从 stdin 或配置文件注入测试请求。
     *
     * 真实实现：
     *   struct {
     *     struct nlmsghdr nlh;
     *     struct sched_context ctx;
     *   } msg;
     *   recv(fd, &msg, sizeof(msg), 0);
     */
    (void)fd;
    (void)request_id;
    (void)domain;
    (void)feat;
    (void)feat_len;
    (void)layer1_dec;
    (void)layer1_conf;
    return -1; /* 无数据 */
}

/*
 * 模拟：发送决策到内核
 * 真实实现：通过 netlink 发送
 */
static int nl_send_decision(int fd,
                             unsigned long long request_id,
                             int domain,
                             unsigned char decision,
                             unsigned char confidence,
                             const char *reason,
                             int override)
{
    /*
     * 真实实现：
     *   struct nlmsghdr nlh = {
     *     .nlmsg_type = AI_L2_CMD_DECISION_RES,
     *     .nlmsg_seq  = request_id,
     *     .nlmsg_pid  = getpid(),
     *   };
     *   struct ai_layer2_decision res = {
     *     .request_id = request_id,
     *     .domain     = domain,
     *     .decision   = decision,
     *     .confidence = confidence,
     *     .override_layer1 = override,
     *   };
     *   send(fd, &nlh, sizeof(nlh), 0);
     *   send(fd, &res, sizeof(res), 0);
     */
    (void)fd;
    (void)request_id;
    (void)domain;
    (void)decision;
    (void)confidence;
    (void)reason;
    (void)override;

    stats.nl_sent++;
    return 0;
}

/* =========================================================================
 * 推理线程
 * ========================================================================= */

static pthread_mutex_t queue_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_cond  = PTHREAD_COND_INITIALIZER;

typedef struct {
    unsigned long long  request_id;
    unsigned long long  timestamp_ms;
    int                 domain;
    unsigned char       feat[512];
    size_t              feat_len;
    unsigned char       layer1_decision;
    int                 layer1_confidence;
    struct Req         *next;
} Req;

static Req *req_head = NULL, *req_tail = NULL;
static int  req_count = 0;
static volatile int running = 1;

static void req_enqueue(unsigned long long req_id, int domain,
                        const unsigned char *feat, size_t feat_len,
                        unsigned char l1_dec, int l1_conf)
{
    Req *r = xcalloc(1, sizeof(Req));
    r->request_id = req_id;
    r->timestamp_ms = now_ms();
    r->domain = domain;
    if (feat_len > sizeof(r->feat)) feat_len = sizeof(r->feat);
    memcpy(r->feat, feat, feat_len);
    r->feat_len = feat_len;
    r->layer1_decision = l1_dec;
    r->layer1_confidence = l1_conf;

    pthread_mutex_lock(&queue_lock);
    if (req_tail) req_tail->next = r;
    else req_head = r;
    req_tail = r;
    req_count++;
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_lock);
}

static Req *req_dequeue(void)
{
    pthread_mutex_lock(&queue_lock);
    while (!req_head && running)
        pthread_cond_wait(&queue_cond, &queue_lock);

    if (!running && !req_head) {
        pthread_mutex_unlock(&queue_lock);
        return NULL;
    }

    Req *r = req_head;
    req_head = r->next;
    if (!req_head) req_tail = NULL;
    req_count--;
    pthread_mutex_unlock(&queue_lock);
    return r;
}

/* 推理工作线程 */
static void *infer_worker(void *arg)
{
    (void)arg;
    LOGI("Inference worker thread started");

    while (running) {
        Req *r = req_dequeue();
        if (!r) break;

        stats.decisions_sent++;

        unsigned char decision, confidence;
        char reason[256];
        unsigned long long latency_ns;

        int ret = do_inference(r->domain,
                               r->feat, r->feat_len,
                               r->layer1_decision, r->layer1_confidence,
                               &decision, &confidence,
                               reason, sizeof(reason),
                               &latency_ns);

        if (ret == 0) {
            stats.decisions_received++;

            /* 判断是否覆盖 Layer1 */
            int threshold = cfg.sched_threshold;
            if (r->domain == DOMAIN_IO) threshold = cfg.io_threshold;
            if (r->domain == DOMAIN_SEC) threshold = cfg.sec_threshold;

            int override = 0;
            if (cfg.mode == MODE_OVERRIDE &&
                (int)confidence * 100 >= threshold) {
                override = 1;
                stats.override_layer1++;
            }

            /* 发送结果 */
            if (nl_client.valid)
                nl_send_decision(nl_client.fd, r->request_id, r->domain,
                                decision, confidence, reason, override);

            /* 日志 */
            const char *dec_str = (r->domain == DOMAIN_SCHED) ?
                decision_str_sched(decision) :
                (r->domain == DOMAIN_IO) ?
                decision_str_io(decision) :
                decision_str_sec(decision);

            const char *backend_name = cfg.backend == BACKEND_DEEPSEEK ?
                "DeepSeek" : "Kimi K3";

            LOGI("[%s] req=%llu domain=%d dec=%s conf=%d "
                 "lat=%llums override=%d reason=%.64s",
                 backend_name, r->request_id, r->domain,
                 dec_str, confidence,
                 latency_ns / 1000000ULL, override, reason);
        } else {
            LOGE("Inference failed for req=%llu domain=%d",
                 r->request_id, r->domain);
        }

        free(r);
    }

    return NULL;
}

/* =========================================================================
 * 测试/演示模式（无需真实内核）
 * ========================================================================= */

/* 模拟任务特征（用于演示） */
static const unsigned char demo_feat_sched[64] = {
    0x01, 0x00, 0x00, 0x00, /* sum_exec_runtime high */
    0x19, 0x00, 0x00, 0x00, /* nvcsw = 25 */
    0x05, 0x00, 0x00, 0x00, /* nivcsw = 5 */
    0x00, 0x03, 0x00, 0x00, /* cpu_util = 768 */
    0x00, 0x00, 0xE0, 0x01, /* io_wait_ns = 200000000 */
    0x20, 0x00, 0x00, 0x00, /* mem_usage_kb = 32MB */
    0x78, 0x00, 0x00, 0x00, /* prio = 120 */
    't', 'e', 's', 't',     /* comm = "test" */
    't', '_', 'a', 'i', 0x00,
};

static const unsigned char demo_feat_io[64] = {
    0xC0, 0xA8, 0x01, 0x64, /* src_ip 192.168.1.100 */
    0x08, 0x08, 0x08, 0x08, /* dst_ip 8.8.8.8 */
    0x00, 0x50, 0x00, 0x35, /* src=80 dst=53 */
    17, 0x00, 0x00, 0x28,   /* protocol=UDP len=40 */
    0x00, 0x02, 0x00, 0x00, /* tcp_flags=SYN */
    0x40, 0x01, 0x00, 0x00, /* ttl=64 tos=0 */
    0x00, 0x00, 0x00, 0x00,
};

static void run_demo_mode(void)
{
    LOGI("");
    LOGI("==============================================");
    LOGI("  AI Linux Layer2 Gateway — DEMO MODE");
    LOGI("  后端: %s | 模型: %s",
         cfg.backend == BACKEND_DEEPSEEK ? "DeepSeek" : "Kimi K3",
         cfg.model);
    LOGI("  模式: %s",
         cfg.mode == MODE_ASYNC ? "异步" :
         cfg.mode == MODE_SYNC ? "同步" : "覆盖");
    LOGI("==============================================");
    LOGI("");

    if (!cfg.api_key[0]) {
        LOGE("API key not set! 请设置 DeepSeek 或 Kimi API Key.");
        LOGE("  DeepSeek: --key YOUR_DEEPSEEK_KEY");
        LOGE("  Kimi K3:  --backend kimi --key YOUR_KIMI_KEY");
        LOGE("");
        LOGE("  或设置环境变量：");
        LOGE("  export DEEPSEEK_API_KEY=your_key");
        LOGE("  export KIMI_API_KEY=your_key");
        return;
    }

    /* 启动推理线程 */
    pthread_t worker;
    pthread_create(&worker, NULL, infer_worker, NULL);

    /* 模拟 3 个调度决策请求 */
    for (int i = 1; i <= 3; i++) {
        LOGI("--- 发送测试请求 #%d ---", i);

        unsigned char feat[64];
        int domain = (i % 2 == 0) ? DOMAIN_IO : DOMAIN_SCHED;
        size_t feat_len = (i % 2 == 0) ? sizeof(demo_feat_io) : sizeof(demo_feat_sched);

        if (i % 2 == 0)
            memcpy(feat, demo_feat_io, feat_len);
        else
            memcpy(feat, demo_feat_sched, feat_len);

        req_enqueue(i, domain, feat, feat_len, DEC_KEEP, 55);

        sleep(1);
    }

    /* 等待队列处理完成 */
    sleep(5);

    running = 0;
    pthread_cond_broadcast(&queue_cond);
    pthread_join(worker, NULL);

    /* 打印统计 */
    LOGI("");
    LOGI("==============================================");
    LOGI("  运行统计");
    LOGI("==============================================");
    LOGI("  API 调用总数:       %llu", stats.api_calls);
    LOGI("  API 成功:           %llu", stats.api_success);
    LOGI("  API 错误:           %llu", stats.api_errors);
    LOGI("  DeepSeek 调用:      %llu", stats.deepseek_calls);
    LOGI("  Kimi K3 调用:       %llu", stats.kimi_calls);
    LOGI("  决策请求接收:       %llu", stats.decisions_sent);
    LOGI("  决策响应发送:       %llu", stats.decisions_received);
    LOGI("  Layer2 覆盖 Layer1: %llu", stats.override_layer1);
    LOGI("  缓存命中:          %llu", stats.cache_hits);
    LOGI("  缓存未命中:        %llu", stats.cache_misses);
    LOGI("==============================================");
}

/* =========================================================================
 * 信号处理
 * ========================================================================= */

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
    pthread_cond_broadcast(&queue_cond);
}

/* =========================================================================
 * 配置解析
 * ========================================================================= */

static void usage(const char *prog)
{
    printf("\n");
    printf("  AI Linux Layer2 Gateway — DeepSeek & Kimi K3\n");
    printf("  版本: %s | 编译: %s\n\n", VERSION, BUILD_DATE);
    printf("  用法:\n");
    printf("    %s [选项]\n\n", prog);
    printf("  后端选项:\n");
    printf("    --backend BACKEND   后端: deepseek(默认) / kimi\n");
    printf("    --key KEY           API 密钥\n");
    printf("    --key-env VAR       从环境变量读取密钥\n");
    printf("    --model MODEL       模型名\n");
    printf("\n");
    printf("  DeepSeek 模型:\n");
    printf("    --model deepseek-chat (默认)\n");
    printf("    --model deepseek-coder\n");
    printf("    --model deepseek-reasoner\n");
    printf("\n");
    printf("  Kimi K3 模型:\n");
    printf("    --model moonshot-v1-8k (默认)\n");
    printf("    --model moonshot-v1-32k\n");
    printf("    --model moonshot-v1-128k\n");
    printf("\n");
    printf("  运行模式:\n");
    printf("    --mode MODE         async(默认) / sync / override\n");
    printf("\n");
    printf("  阈值:\n");
    printf("    --sched-threshold N 调度置信度阈值 (0-10000, 默认 7000)\n");
    printf("    --io-threshold N     IO 置信度阈值 (默认 7000)\n");
    printf("    --sec-threshold N    安全置信度阈值 (默认 8000)\n");
    printf("\n");
    printf("  其他:\n");
    printf("    --timeout MS         API 超时毫秒 (默认 5000)\n");
    printf("    --debug              启用调试日志\n");
    printf("    --demo               运行演示模式（无需内核）\n");
    printf("    --help               显示本帮助\n");
    printf("\n");
    printf("  示例:\n");
    printf("    # 使用 DeepSeek（默认）\n");
    printf("    %s --key $DEEPSEEK_API_KEY --mode async\n", prog);
    printf("\n");
    printf("    # 使用 Kimi K3\n");
    printf("    %s --backend kimi --key $KIMI_API_KEY \\\n", prog);
    printf("         --model moonshot-v1-32k --mode async\n", prog);
    printf("\n");
    printf("    # 演示模式（无需密钥，模拟请求）\n");
    printf("    %s --demo --key fake_key_for_demo\n", prog);
    printf("\n");
}

static void parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i+1 < argc) {
            if (strcmp(argv[i+1], "kimi") == 0) {
                cfg.backend = BACKEND_KIMI;
                strncpy(cfg.model, KIMI_MODEL, sizeof(cfg.model)-1);
            } else {
                cfg.backend = BACKEND_DEEPSEEK;
                strncpy(cfg.model, DEEPSEEK_MODEL, sizeof(cfg.model)-1);
            }
            i++;
        }
        else if (strcmp(argv[i], "--key") == 0 && i+1 < argc) {
            strncpy(cfg.api_key, argv[++i], sizeof(cfg.api_key)-1);
        }
        else if (strcmp(argv[i], "--key-env") == 0 && i+1 < argc) {
            const char *val = getenv(argv[++i]);
            if (val) strncpy(cfg.api_key, val, sizeof(cfg.api_key)-1);
        }
        else if (strcmp(argv[i], "--model") == 0 && i+1 < argc) {
            strncpy(cfg.model, argv[++i], sizeof(cfg.model)-1);
        }
        else if (strcmp(argv[i], "--mode") == 0 && i+1 < argc) {
            if (strcmp(argv[i+1], "sync") == 0)    cfg.mode = MODE_SYNC;
            else if (strcmp(argv[i+1], "override") == 0) cfg.mode = MODE_OVERRIDE;
            else                                     cfg.mode = MODE_ASYNC;
            i++;
        }
        else if (strcmp(argv[i], "--sched-threshold") == 0 && i+1 < argc) {
            cfg.sched_threshold = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--io-threshold") == 0 && i+1 < argc) {
            cfg.io_threshold = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--sec-threshold") == 0 && i+1 < argc) {
            cfg.sec_threshold = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--timeout") == 0 && i+1 < argc) {
            cfg.api_timeout_ms = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--debug") == 0) {
            cfg.debug = 1;
            log_set_level(2);
        }
        else if (strcmp(argv[i], "--demo") == 0) {
            /* 演示模式不连接内核 */
            strncpy(cfg.api_key, "demo_key", sizeof(cfg.api_key)-1);
        }
        else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            exit(0);
        }
    }

    /* 自动从环境变量读取密钥 */
    if (!cfg.api_key[0]) {
        const char *key = getenv("DEEPSEEK_API_KEY");
        if (!key) key = getenv("KIMI_API_KEY");
        if (!key) key = getenv("KIMI_API_KEY");  /* 重复覆盖 */
        if (key) strncpy(cfg.api_key, key, sizeof(cfg.api_key)-1);
    }

    /* 自动选择模型 */
    if (cfg.backend == BACKEND_KIMI) {
        int known = 0;
        for (size_t i = 0; i < sizeof(kimi_models)/sizeof(kimi_models[0]); i++) {
            if (strcmp(cfg.model, kimi_models[i]) == 0) { known = 1; break; }
        }
        if (!known) strncpy(cfg.model, KIMI_MODEL, sizeof(cfg.model)-1);
    } else {
        int known = 0;
        for (size_t i = 0; i < sizeof(deepseek_models)/sizeof(deepseek_models[0]); i++) {
            if (strcmp(cfg.model, deepseek_models[i]) == 0) { known = 1; break; }
        }
        if (!known) strncpy(cfg.model, DEEPSEEK_MODEL, sizeof(cfg.model)-1);
    }
}

/* =========================================================================
 * 主函数
 * ========================================================================= */

int main(int argc, char **argv)
{
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════╗\n");
    printf("  ║   AI Linux — Layer2 AI Gateway                   ║\n");
    printf("  ║   支持 DeepSeek · Kimi K3                        ║\n");
    printf("  ║   v%s | %s              ║\n", VERSION, BUILD_DATE);
    printf("  ╚══════════════════════════════════════════════════════╝\n\n");

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    parse_args(argc, argv);

    stats.start_time_ms = now_ms();

    /* 显示配置 */
    LOGI("后端:   %s",
         cfg.backend == BACKEND_DEEPSEEK ? "DeepSeek" : "Kimi K3");
    LOGI("模型:   %s", cfg.model);
    LOGI("模式:   %s",
         cfg.mode == MODE_SYNC ? "同步" :
         cfg.mode == MODE_OVERRIDE ? "覆盖" : "异步");
    LOGI("调度阈值: %d", cfg.sched_threshold);
    LOGI("IO阈值:   %d", cfg.io_threshold);
    LOGI("安全阈值: %d", cfg.sec_threshold);
    LOGI("API超时:  %d ms", cfg.api_timeout_ms);

    if (!cfg.api_key[0]) {
        LOGE("警告: 未设置 API 密钥！");
        LOGE("  设置环境变量 DEEPSEEK_API_KEY 或 KIMI_API_KEY");
        LOGE("  或使用 --key 参数");
        LOGI("  使用 --demo 模式演示（无需密钥）");
    } else {
        /* 隐藏密钥显示 */
        LOGI("API密钥: %s***%s",
             cfg.api_key[0] < 8 ? "" : cfg.api_key,
             cfg.api_key[0] < 8 ? "" : cfg.api_key + strlen(cfg.api_key) - 4);
    }

    printf("\n");

    /* 检查是否有 --demo 参数 */
    int demo_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) { demo_mode = 1; break; }
    }

    if (demo_mode || (!cfg.api_key[0])) {
        run_demo_mode();
        return 0;
    }

    /* 真实模式：启动推理线程 */
    pthread_t worker;
    pthread_create(&worker, NULL, infer_worker, NULL);

    LOGI("Layer2 gateway 启动，等待内核请求...");

    /*
     * 真实模式：接收来自内核的 netlink 请求
     *
     * 实现路径（待接入真实内核）：
     *   1. 创建 netlink socket
     *   2. 绑定到 NETLINK_GENERIC 族
     *   3. 循环 recv() netlink 消息
     *   4. 解析决策请求
     *   5. req_enqueue() 送入推理队列
     *
     * 示例代码（伪代码）：
     *
     * int nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
     * struct sockaddr_nl addr = {
     *     .nl_family = AF_NETLINK,
     *     .nl_pid    = getpid(),
     *     .nl_groups = 0,
     * };
     * bind(nl_fd, (struct sockaddr *)&addr, sizeof(addr));
     *
     * while (running) {
     *     char buf[65536];
     *     ssize_t len = recv(nl_fd, buf, sizeof(buf), 0);
     *     if (len <= 0) continue;
     *
     *     struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
     *     while (NLMSG_OK(nlh, len)) {
     *         if (nlh->nlmsg_type == AI_L2_CMD_DECISION_REQ) {
     *             struct sched_context *ctx = NLMSG_DATA(nlh);
     *             req_enqueue(nlh->nlmsg_seq, DOMAIN_SCHED,
     *                        (unsigned char *)ctx, sizeof(*ctx),
     *                        ctx->layer1_decision,
     *                        ctx->layer1_score / 100);
     *         }
     *         nlh = NLMSG_NEXT(nlh, len);
     *     }
     * }
     */

    /* 模拟主循环 */
    while (running) {
        sleep(1);

        /* 心跳日志 */
        static int tick = 0;
        tick++;
        if (tick % 30 == 0) {
            unsigned long long uptime = (now_ms() - stats.start_time_ms) / 1000ULL;
            LOGI("[heartbeat] uptime=%llus queue=%d api_calls=%llu "
                 "decisions=%llu deepseek=%llu kimi=%llu",
                 uptime, req_count, stats.api_calls, stats.decisions_received,
                 stats.deepseek_calls, stats.kimi_calls);
        }
    }

    running = 0;
    pthread_cond_broadcast(&queue_cond);
    pthread_join(worker, NULL);

    printf("\n  Layer2 gateway exited.\n\n");
    return 0;
}
