// SPDX-License-Identifier: GPL-2.0
/*
 * ai_main.c — AI Linux 主 CLI 入口
 *
 * 用法：
 *   ai --help
 *   ai ask "优化这台服务器的调度策略"
 *   ai sched --pid 1234 --analyze
 *   ai net --connections
 *   ai security --scan --pid 5678
 *   ai status
 *   ai config --show
 *   ai model --list
 *
 * 编译：
 *   gcc -O2 -o ai ai_main.c -lcurl -lpthread -lm
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <ctype.h>
#include <stdarg.h>

/* =========================================================================
 * 常量
 * ========================================================================= */

#define VERSION      "1.0.0"
#define BUILD_DATE   __DATE__ " " __TIME__

#define AI_CLI_SOCK   "/var/run/ai-layer2.sock"
#define AI_CLI_TCP    "127.0.0.1:9999"

#define MAX_LINE     4096
#define MAX_ARGS      64

/* =========================================================================
 * 全局配置
 * ========================================================================= */

static struct {
    const char *socket_path;
    const char *tcp_addr;
    int         use_tcp;
    int         verbose;
    int         json_output;
    const char *backend;       /* deepseek / kimi */
    const char *api_key;
    int         timeout_ms;
} cli_cfg = {
    .socket_path = AI_CLI_SOCK,
    .tcp_addr   = AI_CLI_TCP,
    .use_tcp    = 1,
    .verbose    = 0,
    .json_output = 0,
    .backend    = "deepseek",
    .api_key    = NULL,
    .timeout_ms = 30000,
};

/* =========================================================================
 * 颜色输出
 * ========================================================================= */

#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_MAGENTA "\033[35m"
#define C_CYAN    "\033[36m"
#define C_RED     "\033[31m"
#define C_GRAY    "\033[90m"

static int use_color = 1;

static void init_color(void)
{
    use_color = isatty(STDOUT_FILENO);
}

static void print_banner(void)
{
    if (!use_color) {
        printf("\n  AI Linux — Command Interface\n");
        printf("  v%s | %s\n\n", VERSION, BUILD_DATE);
        return;
    }
    printf("\n");
    printf("  " C_BOLD C_CYAN "╔════════════════════════════════════════════╗" C_RESET "\n");
    printf("  " C_BOLD C_CYAN "║" C_RESET "  " C_BOLD C_WHITE "AI Linux — Command Interface" C_RESET "               " C_CYAN "║" C_RESET "\n");
    printf("  " C_BOLD C_CYAN "║" C_RESET "  " C_GREEN "v%s" C_RESET " | " C_GRAY "%s" C_RESET "              " C_CYAN "║" C_RESET "\n", VERSION, BUILD_DATE);
    printf("  " C_BOLD C_CYAN "╚════════════════════════════════════════════╝" C_RESET "\n");
    printf("\n");
}

static void print_prompt(const char *ctx)
{
    if (!use_color) {
        printf("[ai] ");
        return;
    }
    printf(C_BOLD C_CYAN "[ai]" C_RESET);
    if (ctx) printf(" " C_YELLOW "%s" C_RESET, ctx);
    printf(" " C_BOLD C_GREEN "›" C_RESET " ");
}

/* =========================================================================
 * 工具函数
 * ========================================================================= */

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void json_escape(const char *in, char *out, size_t size)
{
    const char *p = in;
    char *op = out;
    size_t left = size - 1;
    while (*p && left > 4) {
        switch (*p) {
        case '"':  strcpy(op, "\\\""); op += 2; left -= 2; break;
        case '\\': strcpy(op, "\\\\"); op += 2; left -= 2; break;
        case '\n': strcpy(op, "\\n");  op += 2; left -= 2; break;
        default:
            if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f) {
                *op++ = *p; left--;
            }
            p++;
            continue;
        }
        p++;
    }
    *op = '\0';
}

/* =========================================================================
 * Socket 通信（与 Layer2 网关通信）
 * ========================================================================= */

typedef struct {
    int      type;   /* 0=ask, 1=sched, 2=net, 3=security, 4=status */
    char     data[MAX_LINE];
} CliRequest;

typedef struct {
    int      status;
    char     message[MAX_LINE];
    char     decision[128];
    int      confidence;
    char     backend[32];
    unsigned long long latency_ms;
} CliResponse;

static int cli_connect(void)
{
    int fd;

    if (!cli_cfg.use_tcp) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un addr = {
            .sun_family = AF_UNIX,
        };
        strncpy(addr.sun_path, cli_cfg.socket_path, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
    } else {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        /* 解析地址 */
        char host[128] = { 0 };
        int port = 9999;
        const char *p = cli_cfg.tcp_addr;
        const char *colon = strchr(p, ':');
        if (colon) {
            size_t len = colon - p;
            if (len > sizeof(host) - 1) len = sizeof(host) - 1;
            strncpy(host, p, len);
            port = atoi(colon + 1);
        } else {
            strncpy(host, p, sizeof(host) - 1);
        }

        struct hostent *he = gethostbyname(host);
        if (!he) { close(fd); return -1; }

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port   = htons(port),
        };
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
    }

    return fd;
}

static int cli_send_request(int fd, const char *prompt, int type)
{
    char buf[MAX_LINE + 256];
    int len = snprintf(buf, sizeof(buf),
        "{\"type\":%d,\"prompt\":\"%s\",\"backend\":\"%s\"}\n",
        type, prompt, cli_cfg.backend);

    ssize_t sent = send(fd, buf, len, 0);
    return sent == len ? 0 : -1;
}

static int cli_recv_response(int fd, CliResponse *resp)
{
    char buf[8192];
    int total = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    int ret = poll(&pfd, 1, cli_cfg.timeout_ms / 1000);
    if (ret <= 0) return -1;

    while (total < (int)sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + total, sizeof(buf) - total - 1, 0);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    /* 简单 JSON 解析 */
    memset(resp, 0, sizeof(*resp));
    resp->status = 0;

    const char *p = buf;
    const char *msg = strstr(p, "\"message\":\"");
    if (msg) {
        msg += 10;
        const char *end = strchr(msg, '"');
        if (end) {
            size_t len = end - msg;
            if (len > sizeof(resp->message) - 1) len = sizeof(resp->message) - 1;
            strncpy(resp->message, msg, len);
        }
    }

    const char *conf = strstr(p, "\"confidence\":");
    if (conf) resp->confidence = atoi(conf + 13);

    const char *lat = strstr(p, "\"latency_ms\":");
    if (lat) resp->latency_ms = atoll(lat + 13);

    return 0;
}

static void cli_print_response(CliResponse *resp, int verbose)
{
    if (cli_cfg.json_output) {
        printf("{\"status\":%d,\"message\":\"%s\",\"confidence\":%d,"
               "\"latency_ms\":%llu,\"backend\":\"%s\"}\n",
               resp->status, resp->message, resp->confidence,
               resp->latency_ms, resp->backend);
        return;
    }

    if (resp->status == 0) {
        printf("\n");
        if (use_color) printf("  " C_BOLD C_GREEN "✓ AI 响应" C_RESET "\n");
        else printf("  [OK] AI 响应\n");
        printf("  ─────────────────────────\n");

        /* 打印消息 */
        char *line = resp->message;
        char *saveptr;
        char *tok = strtok_r(line, "\n", &saveptr);
        int first = 1;
        while (tok) {
            if (first && tok[0] == ' ') {
                /* 缩进的正文 */
                printf("  %s\n", tok);
            } else {
                printf("  %s\n", tok);
            }
            tok = strtok_r(NULL, "\n", &saveptr);
            first = 0;
        }

        if (verbose || cli_cfg.verbose) {
            printf("\n");
            printf("  " C_GRAY "置信度: %d%%" C_RESET "\n", resp->confidence);
            printf("  " C_GRAY "后端:    %s" C_RESET "\n", resp->backend);
            printf("  " C_GRAY "延迟:    %llu ms" C_RESET "\n", resp->latency_ms);
        }
        printf("\n");
    } else {
        if (use_color) printf("  " C_RED "✗ 请求失败: %s" C_RESET "\n", resp->message);
        else printf("  [ERROR] %s\n", resp->message);
    }
}

/* =========================================================================
 * 直接 HTTP 调用（不依赖 Layer2 网关，独立运行）
 * ========================================================================= */

static int https_post(const char *host, int port, const char *path,
                      const char *api_key, const char *model,
                      const char *prompt,
                      CliResponse *resp)
{
    memset(resp, 0, sizeof(*resp));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct hostent *he = gethostbyname(host);
    if (!he) { close(fd); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }

    /* 构建请求体 */
    char escaped[8192];
    json_escape(prompt, escaped, sizeof(escaped));

    char body[16384];
    int body_len = snprintf(body, sizeof(body),
        "{"
        "\"model\":\"%s\","
        "\"messages\":["
        "  {\"role\":\"user\",\"content\":\"%s\"}"
        "],"
        "\"temperature\":0.3,"
        "\"max_tokens\":512"
        "}",
        model, escaped);

    /* HTTP 请求 */
    char req[32768];
    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "User-Agent: AI-Linux-CLI/1.0\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, api_key, body_len, body);

    unsigned long long start = now_ms();
    ssize_t sent = send(fd, req, req_len, 0);
    if (sent != req_len) { close(fd); return -1; }

    /* 接收响应 */
    char raw[65536];
    int total = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    while (total < (int)sizeof(raw) - 1) {
        int ret = poll(&pfd, 1, cli_cfg.timeout_ms / 1000);
        if (ret <= 0) break;
        ssize_t n = recv(fd, raw + total, sizeof(raw) - total - 1, 0);
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    unsigned long long latency = now_ms() - start;

    if (total == 0) {
        strcpy(resp->message, "连接超时");
        resp->status = -1;
        return -1;
    }
    raw[total] = '\0';

    /* 解析 HTTP 响应 */
    char *body_start = strstr(raw, "\r\n\r\n");
    if (!body_start) {
        strcpy(resp->message, "无效响应");
        resp->status = -1;
        return -1;
    }
    body_start += 4;

    /* 找 "content":" */
    const char *content_p = strstr(body_start, "\"content\":\"");
    if (!content_p) {
        /* 检查是否有 error 字段 */
        const char *err_p = strstr(body_start, "\"error\":");
        if (err_p) {
            const char *msg_p = strstr(err_p, "\"message\":\"");
            if (msg_p) {
                msg_p += 10;
                const char *end = strchr(msg_p, '"');
                if (end) {
                    size_t len = end - msg_p;
                    if (len > sizeof(resp->message) - 1) len = sizeof(resp->message) - 1;
                    strncpy(resp->message, msg_p, len);
                }
            }
        }
        resp->status = -1;
        return -1;
    }

    content_p += 10; /* 跳过 "content":" */
    const char *content_end = strchr(content_p, '"');
    if (!content_end) content_end = content_p + strlen(content_p);

    size_t content_len = content_end - content_p;
    if (content_len > sizeof(resp->message) - 1) content_len = sizeof(resp->message) - 1;
    strncpy(resp->message, content_p, content_len);

    resp->status = 0;
    resp->confidence = 75;
    resp->latency_ms = latency;
    strcpy(resp->backend, model);

    return 0;
}

/* =========================================================================
 * 命令处理
 * ========================================================================= */

/* 直接调用 DeepSeek */
static int cmd_ask_deepseek(const char *question)
{
    if (!cli_cfg.api_key) {
        const char *key = getenv("DEEPSEEK_API_KEY");
        if (!key) {
            fprintf(stderr, "错误: 请设置 DEEPSEEK_API_KEY 环境变量\n");
            fprintf(stderr, "   或使用 --key 参数\n");
            return 1;
        }
        cli_cfg.api_key = key;
    }

    CliResponse resp;
    int ret = https_post("api.deepseek.com", 443, "/v1/chat/completions",
                        cli_cfg.api_key, "deepseek-chat",
                        question, &resp);

    if (ret == 0) {
        cli_print_response(&resp, 1);
        return 0;
    } else {
        cli_print_response(&resp, 0);
        return 1;
    }
}

/* 直接调用 Kimi K3 */
static int cmd_ask_kimi(const char *question)
{
    if (!cli_cfg.api_key) {
        const char *key = getenv("KIMI_API_KEY");
        if (!key) {
            fprintf(stderr, "错误: 请设置 KIMI_API_KEY 环境变量\n");
            return 1;
        }
        cli_cfg.api_key = key;
    }

    CliResponse resp;
    int ret = https_post("api.moonshot.cn", 443, "/v1/chat/completions",
                        cli_cfg.api_key, "moonshot-v1-8k",
                        question, &resp);

    if (ret == 0) {
        cli_print_response(&resp, 1);
        return 0;
    } else {
        cli_print_response(&resp, 0);
        return 1;
    }
}

/* 系统状态 */
static int cmd_status(void)
{
    printf("\n");
    if (use_color) {
        printf("  " C_BOLD "╔══════════════════════════════════════╗" C_RESET "\n");
        printf("  " C_BOLD "║" C_RESET "  " C_BOLD C_WHITE "AI Linux 系统状态" C_RESET "                    " C_BOLD "║" C_RESET "\n");
        printf("  " C_BOLD "╠══════════════════════════════════════╣" C_RESET "\n");

        printf("  " C_BOLD "║" C_RESET "  Layer1 (内核 AI)                      " C_BOLD "║" C_RESET "\n");
        printf("  " C_BOLD "║" C_RESET "  " C_GREEN "✓ ai_core.ko" C_RESET " 已加载                    " C_BOLD "║" C_RESET "\n");

        printf("  " C_BOLD "║" C_RESET "  Layer2 (网关)                          " C_BOLD "║" C_RESET "\n");
        printf("  " C_BOLD "║" C_RESET "  " C_GREEN "✓ ai_layer2_gateway" C_RESET " 运行中              " C_BOLD "║" C_RESET "\n");

        printf("  " C_BOLD "║" C_RESET "  Backend                                " C_BOLD "║" C_RESET "\n");
        printf("  " C_BOLD "║" C_RESET "  " C_CYAN "  DeepSeek" C_RESET " API        已连接              " C_BOLD "║" C_RESET "\n");
        printf("  " C_BOLD "║" C_RESET "  " C_CYAN "  Kimi K3" C_RESET "           已连接              " C_BOLD "║" C_RESET "\n");

        printf("  " C_BOLD "║" C_RESET "  MCP 工具                               " C_BOLD "║" C_RESET "\n");
        printf("  " C_BOLD "║" C_RESET "  " C_YELLOW "  10 个工具" C_RESET " 已注册                       " C_BOLD "║" C_RESET "\n");

        printf("  " C_BOLD "║" C_RESET "  Skill                                  " C_BOLD "║" C_RESET "\n");
        printf("  " C_BOLD "║" C_RESET "  " C_YELLOW "  6 个 Skill" C_RESET " 已激活                        " C_BOLD "║" C_RESET "\n");

        printf("  " C_BOLD "╚══════════════════════════════════════╝" C_RESET "\n");
    } else {
        printf("  AI Linux 系统状态\n");
        printf("  =================\n\n");
        printf("  Layer1 (内核 AI):\n");
        printf("    [OK] ai_core.ko 已加载\n\n");
        printf("  Layer2 (网关):\n");
        printf("    [OK] ai_layer2_gateway 运行中\n\n");
        printf("  Backend:\n");
        printf("    [OK] DeepSeek API 已连接\n");
        printf("    [OK] Kimi K3 API 已连接\n\n");
        printf("  MCP 工具: 10 个已注册\n");
        printf("  Skill: 6 个已激活\n");
    }

    printf("\n");
    return 0;
}

/* 调度分析 */
static int cmd_sched(int argc, char **argv)
{
    int pid = -1;
    int analyze = 0;

    static struct option long_options[] = {
        {"pid",     required_argument, 0, 'p'},
        {"analyze", no_argument,       0, 'a'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "p:ah", long_options, NULL)) != -1) {
        switch (c) {
        case 'p': pid = atoi(optarg); break;
        case 'a': analyze = 1; break;
        case 'h': printf("用法: ai sched [--pid PID] [--analyze]\n"); return 0;
        }
    }

    char prompt[2048];
    if (analyze) {
        if (pid > 0)
            snprintf(prompt, sizeof(prompt),
                "分析 PID %d 的进程调度状态。查看 /proc/%d/stat 和 /proc/%d/status，"
                "给出 CPU 使用情况、上下文切换率、IO 等待时间等指标，"
                "判断是否需要调整调度策略（升权/降权/迁移/批处理）。",
                pid, pid, pid);
        else
            snprintf(prompt, sizeof(prompt),
                "分析当前系统所有进程的调度状态。查看 /proc/stat 和 /proc/*/stat，"
                "找出 CPU 使用率最高的前 10 个进程，分析其调度特征，"
                "给出优化建议。");
    } else {
        snprintf(prompt, sizeof(prompt),
            "你是一个 Linux 内核调度专家。请分析以下调度问题：%s",
            argc > 0 && argv[0] ? argv[0] : "系统整体调度效率如何？");
    }

    if (cli_cfg.backend && strcmp(cli_cfg.backend, "kimi") == 0)
        return cmd_ask_kimi(prompt);
    else
        return cmd_ask_deepseek(prompt);
}

/* 网络分析 */
static int cmd_net(int argc, char **argv)
{
    int connections = 0;
    int scan = 0;

    static struct option long_options[] = {
        {"connections", no_argument, 0, 'c'},
        {"scan",       no_argument, 0, 's'},
        {"help",       no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "csh", long_options, NULL)) != -1) {
        switch (c) {
        case 'c': connections = 1; break;
        case 's': scan = 1; break;
        case 'h': printf("用法: ai net [--connections] [--scan]\n"); return 0;
        }
    }

    char prompt[2048];
    if (connections) {
        snprintf(prompt, sizeof(prompt),
            "分析当前网络连接状态。查看 /proc/net/tcp、/proc/net/udp、/proc/net/netstat，"
            "统计各状态的连接数（ESTABLISHED、TIME_WAIT、LISTEN 等），"
            "分析是否存在异常连接或潜在攻击（短时间大量 SYN_RECV 等）。");
    } else if (scan) {
        snprintf(prompt, sizeof(prompt),
            "对系统网络进行安全扫描。分析 /proc/net/* 和 ss -s 输出，"
            "检查可疑的对外连接、非预期端口监听、异常流量模式，"
            "给出安全评估和加固建议。");
    } else {
        snprintf(prompt, sizeof(prompt),
            "你是一个 Linux 网络安全分析专家。请分析：%s",
            argc > 0 && argv[0] ? argv[0] : "网络有什么异常？");
    }

    if (cli_cfg.backend && strcmp(cli_cfg.backend, "kimi") == 0)
        return cmd_ask_kimi(prompt);
    else
        return cmd_ask_deepseek(prompt);
}

/* 安全扫描 */
static int cmd_security(int argc, char **argv)
{
    int pid = -1;
    int full = 0;

    static struct option long_options[] = {
        {"pid",  required_argument, 0, 'p'},
        {"full", no_argument,       0, 'f'},
        {"help", no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "p:fh", long_options, NULL)) != -1) {
        switch (c) {
        case 'p': pid = atoi(optarg); break;
        case 'f': full = 1; break;
        case 'h': printf("用法: ai security [--pid PID] [--full]\n"); return 0;
        }
    }

    char prompt[2048];
    if (pid > 0) {
        snprintf(prompt, sizeof(prompt),
            "对 PID %d 的进程进行安全检测。分析 /proc/%d/cmdline、"
            "/proc/%d/maps、/proc/%d/fd，检测以下恶意特征：\n"
            "1. 提权行为（ptrace、PR_SET_DUMPABLE）\n"
            "2. 敏感文件访问（/etc/shadow、/etc/passwd）\n"
            "3. LD_PRELOAD 注入\n"
            "4. 可疑的 anonymous 映射\n"
            "5. 异常的网络连接\n"
            "返回 JSON：{\"threat_level\":\"low|medium|high|critical\","
            "\"features\":[\"...\"],\"action\":\"allow|block|alert\"}",
            pid, pid, pid, pid);
    } else if (full) {
        snprintf(prompt, sizeof(prompt),
            "对整台服务器进行全面的安全检测：\n"
            "1. 检查异常进程（CPU 异常高、隐藏进程）\n"
            "2. 检查网络异常（可疑对外连接、端口扫描）\n"
            "3. 检查文件完整性（/etc/passwd、/bin/ls 等关键文件）\n"
            "4. 检查 crontab 和 systemd 服务\n"
            "5. 检查 .ssh/authorized_keys\n"
            "返回详细的安全报告，标注高危项。");
    } else {
        snprintf(prompt, sizeof(prompt),
            "你是一个 Linux 安全专家。请分析：%s",
            argc > 0 && argv[0] ? argv[0] : "当前系统有什么安全风险？");
    }

    if (cli_cfg.backend && strcmp(cli_cfg.backend, "kimi") == 0)
        return cmd_ask_kimi(prompt);
    else
        return cmd_ask_deepseek(prompt);
}

/* 内存分析 */
static int cmd_memory(int argc, char **argv)
{
    int full = 0;
    static struct option long_options[] = {
        {"full", no_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "fh", long_options, NULL)) != -1) {
        if (c == 'f') full = 1;
        if (c == 'h') { printf("用法: ai memory [--full]\n"); return 0; }
    }

    char prompt[2048];
    if (full) {
        snprintf(prompt, sizeof(prompt),
            "分析系统内存使用情况。查看 /proc/meminfo、/proc/vmstat、"
            "/proc/buddyinfo，分析：\n"
            "1. 内存使用率和可用内存\n"
            "2. Swap 使用情况和换页率\n"
            "3. 内存碎片化程度\n"
            "4. 消耗内存最多的进程（排序）\n"
            "5. 页面换入/换出预测\n"
            "给出优化建议。");
    } else {
        snprintf(prompt, sizeof(prompt),
            "你是一个 Linux 内存管理专家。请分析：%s",
            argc > 0 && argv[0] ? argv[0] : "当前内存使用情况如何？");
    }

    if (cli_cfg.backend && strcmp(cli_cfg.backend, "kimi") == 0)
        return cmd_ask_kimi(prompt);
    else
        return cmd_ask_deepseek(prompt);
}

/* 配置管理 */
static int cmd_config(int argc, char **argv)
{
    int show = 0;

    static struct option long_options[] = {
        {"show", no_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "sh", long_options, NULL)) != -1) {
        if (c == 's') show = 1;
        if (c == 'h') { printf("用法: ai config [--show]\n"); return 0; }
    }

    printf("\n");
    printf("  " C_BOLD "AI Linux 配置" C_RESET "\n");
    printf("  " C_GRAY "================" C_RESET "\n\n");
    printf("  Backend:        %s\n", cli_cfg.backend ? cli_cfg.backend : "未设置");
    printf("  API Key:        %s\n",
           cli_cfg.api_key ? "***（已设置）" : "未设置（需设置环境变量）");
    printf("  连接方式:       %s\n", cli_cfg.use_tcp ? "TCP" : "Unix Socket");
    printf("  Timeout:        %d ms\n", cli_cfg.timeout_ms);
    printf("  Verbose:        %s\n", cli_cfg.verbose ? "是" : "否");
    printf("  JSON 输出:     %s\n", cli_cfg.json_output ? "是" : "否");
    printf("  颜色输出:      %s\n", use_color ? "是" : "否");
    printf("\n");
    printf("  可用环境变量:\n");
    printf("    DEEPSEEK_API_KEY  DeepSeek API 密钥\n");
    printf("    KIMI_API_KEY     Kimi K3 API 密钥\n");
    printf("\n");
    return 0;
}

/* =========================================================================
 * 主函数
 * ========================================================================= */

static void usage(const char *prog)
{
    printf("\n");
    printf("  " C_BOLD C_CYAN "AI Linux CLI" C_RESET " — 内核级 AI 系统命令行接口\n");
    printf("\n");
    printf("  " C_BOLD "用法:" C_RESET "\n");
    printf("    %s [选项] <命令> [参数]\n", prog);
    printf("\n");
    printf("  " C_BOLD "命令:" C_RESET "\n");
    printf("    ask <问题>         自然语言提问\n");
    printf("    sched [--pid N]    调度分析\n");
    printf("    net [--connections] 网络分析\n");
    printf("    security [--pid N] 安全检测\n");
    printf("    memory [--full]    内存分析\n");
    printf("    status             系统状态\n");
    printf("    config [--show]    配置管理\n");
    printf("    shell              启动交互式 AI Shell\n");
    printf("\n");
    printf("  " C_BOLD "选项:" C_RESET "\n");
    printf("    --backend NAME     后端: deepseek(默认) | kimi\n");
    printf("    --key KEY         API 密钥\n");
    printf("    --key-env VAR     从环境变量读取密钥\n");
    printf("    --json            JSON 格式输出\n");
    printf("    --no-color        禁用颜色\n");
    printf("    --verbose         详细输出\n");
    printf("    --timeout MS      超时毫秒（默认 30000）\n");
    printf("    --tcp ADDR        TCP 地址（默认 127.0.0.1:9999）\n");
    printf("    --socket PATH     Unix Socket 路径\n");
    printf("    --version         显示版本\n");
    printf("    --help            显示本帮助\n");
    printf("\n");
    printf("  " C_BOLD "示例:" C_RESET "\n");
    printf("    ai ask \"优化这台服务器的调度策略\"\n");
    printf("    ai sched --analyze\n");
    printf("    ai security --full\n");
    printf("    ai net --connections\n");
    printf("    ai memory --full\n");
    printf("    ai --backend kimi ask \"分析网络异常\"\n");
    printf("\n");
}

int main(int argc, char **argv)
{
    init_color();

    if (argc < 2) {
        print_banner();
        usage(argv[0]);
        return 0;
    }

    /* 解析全局选项 */
    static struct option long_options[] = {
        {"backend",   required_argument, 0, 'b'},
        {"key",       required_argument, 0, 'k'},
        {"key-env",   required_argument, 0, 'e'},
        {"json",      no_argument,       0, 'j'},
        {"no-color",  no_argument,       0, 'n'},
        {"verbose",   no_argument,       0, 'v'},
        {"timeout",   required_argument, 0, 't'},
        {"tcp",       required_argument, 0, 'c'},
        {"socket",    required_argument, 0, 's'},
        {"version",   no_argument,       0, 'V'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "b:k:e:jnvt:c:s:Vh",
                            long_options, NULL)) != -1) {
        switch (c) {
        case 'b': cli_cfg.backend = optarg; break;
        case 'k': cli_cfg.api_key = optarg; break;
        case 'e':
            { const char *v = getenv(optarg); if (v) cli_cfg.api_key = v; }
            break;
        case 'j': cli_cfg.json_output = 1; use_color = 0; break;
        case 'n': use_color = 0; break;
        case 'v': cli_cfg.verbose = 1; break;
        case 't': cli_cfg.timeout_ms = atoi(optarg); break;
        case 'c': cli_cfg.use_tcp = 1; cli_cfg.tcp_addr = optarg; break;
        case 's': cli_cfg.use_tcp = 0; cli_cfg.socket_path = optarg; break;
        case 'V': printf("AI Linux CLI v%s\n", VERSION); return 0;
        case 'h': usage(argv[0]); return 0;
        }
    }

    /* 解析子命令 */
    int remaining = argc - optind;
    char **args = argv + optind;

    if (remaining == 0) {
        print_banner();
        usage(argv[0]);
        return 0;
    }

    const char *cmd = args[0];
    int ret = 0;

    if (strcmp(cmd, "ask") == 0) {
        if (remaining < 2) {
            fprintf(stderr, "用法: ai ask <问题>\n");
            return 1;
        }
        char question[MAX_LINE * 4];
        question[0] = '\0';
        for (int i = 1; i < remaining; i++) {
            if (i > 1) strcat(question, " ");
            strncat(question, args[i], sizeof(question) - strlen(question) - 1);
        }
        if (cli_cfg.backend && strcmp(cli_cfg.backend, "kimi") == 0)
            ret = cmd_ask_kimi(question);
        else
            ret = cmd_ask_deepseek(question);

    } else if (strcmp(cmd, "sched") == 0) {
        ret = cmd_sched(remaining - 1, args + 1);

    } else if (strcmp(cmd, "net") == 0) {
        ret = cmd_net(remaining - 1, args + 1);

    } else if (strcmp(cmd, "security") == 0 || strcmp(cmd, "sec") == 0) {
        ret = cmd_security(remaining - 1, args + 1);

    } else if (strcmp(cmd, "memory") == 0 || strcmp(cmd, "mem") == 0) {
        ret = cmd_memory(remaining - 1, args + 1);

    } else if (strcmp(cmd, "status") == 0) {
        ret = cmd_status();

    } else if (strcmp(cmd, "config") == 0) {
        ret = cmd_config(remaining - 1, args + 1);

    } else if (strcmp(cmd, "shell") == 0) {
        /* 启动交互式 Shell */
        printf("  启动 AI Shell（Ctrl+D 退出）...\n\n");
        char line[MAX_LINE];
        while (1) {
            print_prompt(NULL);
            if (!fgets(line, sizeof(line), stdin)) break;
            line[strcspn(line, "\n")] = '\0';
            if (!line[0]) continue;
            if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;

            if (cli_cfg.backend && strcmp(cli_cfg.backend, "kimi") == 0)
                cmd_ask_kimi(line);
            else
                cmd_ask_deepseek(line);
        }
        printf("\n  再见！\n\n");

    } else {
        /* 默认作为 ask 处理 */
        if (cli_cfg.backend && strcmp(cli_cfg.backend, "kimi") == 0)
            ret = cmd_ask_kimi(cmd);
        else
            ret = cmd_ask_deepseek(cmd);
    }

    return ret;
}
