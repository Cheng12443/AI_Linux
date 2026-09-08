// SPDX-License-Identifier: GPL-2.0
/*
 * ai_web.c — AI Linux Web Dashboard
 *
 * 一个轻量级 HTTP 服务器，提供实时 Web 界面：
 *   - 实时系统状态
 *   - AI 决策日志
 *   - 交互式 AI 问答
 *   - 调度/网络/安全视图
 *   - Skill 路由可视化
 *
 * 编译：
 *   gcc -O2 -o ai-web ai_web.c -lpthread -lm
 *
 * 运行：
 *   ./ai-web --port 8080 --key $DEEPSEEK_API_KEY
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

/* =========================================================================
 * 常量
 * ========================================================================= */

#define VERSION   "1.0.0"
#define PORT      8080
#define MAX_FD   4096
#define MAX_EVENTS 1024
#define MAX_PATH  512

/* =========================================================================
 * 全局配置
 * ========================================================================= */

static struct {
    int         port;
    int         running;
    const char *api_key;
    const char *backend;
    int         verbose;
} web_cfg = {
    .port    = PORT,
    .api_key = NULL,
    .backend = "deepseek",
    .verbose = 0,
};

/* =========================================================================
 * 工具
 * ========================================================================= */

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void html_escape(const char *in, char *out, size_t size)
{
    const char *p = in;
    char *op = out;
    size_t left = size - 1;
    while (*p && left > 8) {
        switch (*p) {
        case '<': strcpy(op, "&lt;");  op += 4; left -= 4; break;
        case '>': strcpy(op, "&gt;");  op += 4; left -= 4; break;
        case '&': strcpy(op, "&amp;"); op += 5; left -= 5; break;
        case '"': strcpy(op, "&quot;");op += 6; left -= 6; break;
        case '\'':strcpy(op, "&#39;"); op += 5; left -= 5; break;
        default:
            if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f)
                { *op++ = *p; left--; }
            else
                { snprintf(op, left, "&#x%02x;", (unsigned char)*p); op += 7; left -= 7; }
        }
        p++;
    }
    *op = '\0';
}

static int json_get_str(const char *json, const char *key, char *out, size_t size)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = end - p;
    if (len > size - 1) len = size - 1;
    strncpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* =========================================================================
 * HTTP 服务器
 * ========================================================================= */

typedef struct {
    int       fd;
    int       events;
    char      buf[32768];
    int       buf_len;
    int       buf_pos;
    int       is_ssl;
    time_t    last_active;
} Conn;

static Conn *conns[MAX_FD];
static int epoll_fd;
static pthread_mutex_t conns_lock = PTHREAD_MUTEX_INITIALIZER;

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }

    if (listen(fd, 128) < 0) {
        close(fd); return -1;
    }

    set_nonblock(fd);
    return fd;
}

static int add_conn(int fd)
{
    pthread_mutex_lock(&conns_lock);
    if (fd >= MAX_FD || conns[fd]) {
        pthread_mutex_unlock(&conns_lock);
        return -1;
    }
    Conn *c = calloc(1, sizeof(Conn));
    c->fd = fd;
    c->last_active = time(NULL);
    conns[fd] = c;

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = fd,
    };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    pthread_mutex_unlock(&conns_lock);
    return 0;
}

static void remove_conn(int fd)
{
    pthread_mutex_lock(&conns_lock);
    if (fd < MAX_FD && conns[fd]) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        free(conns[fd]);
        conns[fd] = NULL;
    }
    pthread_mutex_unlock(&conns_lock);
}

/* =========================================================================
 * HTTP 请求解析
 * ========================================================================= */

typedef struct {
    char method[16];
    char path[MAX_PATH];
    char version[16];
    char query[MAX_PATH];
    char host[128];
    char body[16384];
    int  body_len;
    int  content_length;
} HttpRequest;

static int parse_http_request(const char *data, int len, HttpRequest *req)
{
    memset(req, 0, sizeof(*req));

    const char *line_end = strstr(data, "\r\n");
    if (!line_end) return -1;

    size_t line_len = line_end - data;
    if (line_len >= sizeof(req->method) + sizeof(req->path) + 2)
        return -1;

    char line[1024];
    strncpy(line, data, line_len);
    line[line_len] = '\0';

    /* 解析请求行 */
    if (sscanf(line, "%15s %511s %15s",
                req->method, req->path, req->version) != 3)
        return -1;

    /* 解析 query string */
    char *qm = strchr(req->path, '?');
    if (qm) {
        *qm = '\0';
        strncpy(req->query, qm + 1, sizeof(req->query) - 1);
    }

    /* 解析 headers */
    const char *p = line_end + 2;
    const char *body = strstr(p, "\r\n\r\n");
    if (body) {
        body += 4;
        int body_len = len - (body - data);
        if (body_len > (int)sizeof(req->body) - 1)
            body_len = sizeof(req->body) - 1;
        strncpy(req->body, body, body_len);
        req->body[body_len] = '\0';
        req->body_len = body_len;
    }

    const char *host_p = strstr(p, "Host:");
    if (host_p) {
        host_p += 5;
        while (*host_p == ' ') host_p++;
        const char *end = strstr(host_p, "\r\n");
        if (end) {
            size_t len = end - host_p;
            if (len > sizeof(req->host) - 1) len = sizeof(req->host) - 1;
            strncpy(req->host, host_p, len);
        }
    }

    return 0;
}

/* =========================================================================
 * HTTP 响应
 * ========================================================================= */

static void send_response(int fd, int status, const char *status_text,
                         const char *content_type,
                         const char *body, int body_len,
                         int keepalive)
{
    char header[4096];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "X-Frame-Options: DENY\r\n"
        "\r\n",
        status, status_text,
        content_type, body_len,
        keepalive ? "keep-alive" : "close");

    struct iovec iov[2] = {
        { .iov_base = header, .iov_len = hlen },
        { .iov_base = (void *)body, .iov_len = body_len },
    };

    struct msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = 2,
    };

    sendmsg(fd, &msg, MSG_NOSIGNAL);
}

static void send_file(int fd, const char *filepath, const char *mime)
{
    int fd2 = open(filepath, O_RDONLY);
    if (fd2 < 0) {
        send_response(fd, 404, "Not Found", "text/plain",
                     "404 Not Found", 13, 0);
        return;
    }

    struct stat st;
    fstat(fd2, &st);
    size_t size = st.st_size;

    char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd2, 0);
    close(fd2);

    if (data == MAP_FAILED) {
        send_response(fd, 500, "Internal Server Error", "text/plain",
                     "500 Error", 9, 0);
        return;
    }

    send_response(fd, 200, "OK", mime, data, size, 1);
    munmap(data, size);
}

/* =========================================================================
 * API 调用
 * ========================================================================= */

static int https_post(const char *host, int port, const char *path,
                     const char *api_key, const char *model,
                     const char *prompt,
                     char *result, size_t result_size,
                     unsigned long long *latency_ms)
{
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

    /* JSON body */
    char escaped[8192];
    {
        const char *sp = prompt;
        char *dp = escaped;
        size_t dl = sizeof(escaped) - 1;
        while (*sp && dl > 8) {
            switch (*sp) {
            case '"':  strcpy(dp, "\\\""); dp += 2; dl -= 2; break;
            case '\\': strcpy(dp, "\\\\"); dp += 2; dl -= 2; break;
            case '\n': strcpy(dp, "\\n");  dp += 2; dl -= 2; break;
            default:
                if ((unsigned char)*sp >= 0x20 && (unsigned char)*sp < 0x7f)
                    { *dp++ = *sp; dl--; }
                sp++;
                continue;
            }
            sp++;
        }
        *dp = '\0';
    }

    char body[16384];
    int body_len = snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"temperature\":0.3,\"max_tokens\":512}",
        model, escaped);

    char req[32768];
    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "User-Agent: AI-Linux-Web/1.0\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, api_key, body_len, body);

    unsigned long long start = now_ms();

    ssize_t sent = send(fd, req, req_len, 0);
    if (sent != req_len) { close(fd); return -1; }

    char resp[65536];
    int total = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    while (total < (int)sizeof(resp) - 1) {
        int ret = poll(&pfd, 1, 15000);
        if (ret <= 0) break;
        ssize_t n = recv(fd, resp + total, sizeof(resp) - total - 1, 0);
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    *latency_ms = now_ms() - start;
    resp[total] = '\0';

    if (total == 0) {
        snprintf(result, result_size, "{\"error\":\"连接超时\"}");
        return -1;
    }

    char *body_start = strstr(resp, "\r\n\r\n");
    if (!body_start) {
        snprintf(result, result_size, "{\"error\":\"无效响应\"}");
        return -1;
    }
    body_start += 4;

    /* 提取 content */
    const char *content_p = strstr(body_start, "\"content\":\"");
    if (!content_p) {
        /* 提取 error */
        const char *err_p = strstr(body_start, "\"error\":");
        if (err_p) {
            err_p += 8;
            while (*err_p && *err_p != '"' && *err_p != '{') err_p++;
            const char *end = strchr(err_p, '"');
            if (end) {
                size_t len = end - err_p;
                if (len > result_size - 1) len = result_size - 1;
                strncpy(result, err_p, len);
                result[len] = '\0';
            } else {
                strncpy(result, err_p, result_size - 1);
            }
        } else {
            strncpy(result, body_start, result_size - 1);
        }
        return -1;
    }

    content_p += 11; /* 跳过 "content":" */
    const char *content_end = strchr(content_p, '"');
    if (!content_end) content_end = content_p + strlen(content_p);

    size_t content_len = content_end - content_p;
    if (content_len > result_size - 1) content_len = result_size - 1;
    strncpy(result, content_p, content_len);
    result[content_len] = '\0';

    return 0;
}

/* =========================================================================
 * AI 路由选择
 * ========================================================================= */

static void route_request(const char *prompt, char *backend_out, char *model_out)
{
    const char *lower = prompt;
    char lower_prompt[4096];
    {
        const char *p = prompt;
        char *op = lower_prompt;
        while (*p && (size_t)(op - lower_prompt) < sizeof(lower_prompt) - 1) {
            *op++ = tolower(*p++);
        }
        *op = '\0';
        lower = lower_prompt;
    }

    int score_deepseek = 50;
    int score_kimi = 50;

    if (strstr(lower, "代码") || strstr(lower, "生成") ||
        strstr(lower, "shell") || strstr(lower, "script") ||
        strstr(lower, "compile") || strstr(lower, "编译"))
        score_deepseek += 30;
    if (strstr(lower, "安全") || strstr(lower, "攻击") ||
        strstr(lower, "入侵") || strstr(lower, "threat") ||
        strstr(lower, "malware") || strstr(lower, "漏洞"))
        score_kimi += 20;
    if (strstr(lower, "分析") || strstr(lower, "报告") ||
        strstr(lower, "优化") || strstr(lower, "编排"))
        score_kimi += 15;

    if (score_deepseek >= score_kimi) {
        strcpy(backend_out, "deepseek");
        strcpy(model_out, "deepseek-chat");
        if (strstr(lower, "代码") || strstr(lower, "compile"))
            strcpy(model_out, "deepseek-coder");
    } else {
        strcpy(backend_out, "kimi");
        strcpy(model_out, "moonshot-v1-8k");
    }
}

/* =========================================================================
 * 系统信息（读取 /proc）
 * ========================================================================= */

static void get_cpu_info(char *out, size_t size)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) { snprintf(out, size, "{}"); return; }

    char line[256];
    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0,
                      irq = 0, softirq = 0, steal = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            sscanf(line + 5,
                "%llu %llu %llu %llu %llu %llu %llu %llu",
                &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
            break;
        }
    }
    fclose(fp);

    unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;
    unsigned long long used = total - idle - iowait;
    int usage = total > 0 ? (int)(used * 100 / total) : 0;

    snprintf(out, size,
        "{\"user\":%llu,\"system\":%llu,\"idle\":%llu,\"iowait\":%llu,"
        "\"usage\":%d,\"total\":%llu}",
        user, system, idle, iowait, usage, total);
}

static void get_mem_info(char *out, size_t size)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) { snprintf(out, size, "{}"); return; }

    char line[256];
    unsigned long long mem_total = 0, mem_free = 0, mem_available = 0,
                      swap_total = 0, swap_free = 0, buffers = 0, cached = 0;

    while (fgets(line, sizeof(line), fp)) {
        unsigned long long val;
        if (sscanf(line, "MemTotal: %llu kB", &val) == 1) mem_total = val;
        else if (sscanf(line, "MemFree: %llu kB", &val) == 1) mem_free = val;
        else if (sscanf(line, "MemAvailable: %llu kB", &val) == 1) mem_available = val;
        else if (sscanf(line, "Buffers: %llu kB", &val) == 1) buffers = val;
        else if (sscanf(line, "Cached: %llu kB", &val) == 1) cached = val;
        else if (sscanf(line, "SwapTotal: %llu kB", &val) == 1) swap_total = val;
        else if (sscanf(line, "SwapFree: %llu kB", &val) == 1) swap_free = val;
    }
    fclose(fp);

    int usage = mem_total > 0 ?
        (int)((mem_total - mem_available) * 100 / mem_total) : 0;

    snprintf(out, size,
        "{\"total\":%llu,\"free\":%llu,\"available\":%llu,"
        "\"buffers\":%llu,\"cached\":%llu,\"swap_total\":%llu,"
        "\"swap_free\":%llu,\"usage\":%d}",
        mem_total, mem_free, mem_available, buffers, cached,
        swap_total, swap_free, usage);
}

static void get_loadavg(char *out, size_t size)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) { snprintf(out, size, "[]"); return; }

    char line[128];
    if (fgets(line, sizeof(line), fp)) {
        float l1 = 0, l5 = 0, l15 = 0;
        int run = 0, total = 0;
        sscanf(line, "%f %f %f %d/%d",
               &l1, &l5, &l15, &run, &total);
        snprintf(out, size,
            "{\"load1\":%.2f,\"load5\":%.2f,\"load15\":%.2f,"
            "\"running\":%d,\"total\":%d}", l1, l5, l15, run, total);
    } else {
        snprintf(out, size, "[]");
    }
    fclose(fp);
}

static void get_net_stats(char *out, size_t size)
{
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) { snprintf(out, size, "[]"); return; }

    char line[256];
    char result[4096] = "[";
    int first = 1;

    while (fgets(line, sizeof(line), fp)) {
        if (strchr(line, ':') == NULL) continue;

        char iface[64];
        unsigned long long rx = 0, tx = 0;
        sscanf(strchr(line, ':') + 1, "%llu %*llu %*llu %*llu %*llu %*llu %*llu %*llu %llu",
               &rx, &tx);

        char *dot = strchr(line, ':');
        size_t if_len = dot - line;
        if (if_len > sizeof(iface) - 1) if_len = sizeof(iface) - 1;
        strncpy(iface, line, if_len);
        iface[if_len] = '\0';
        while (*iface == ' ') memmove(iface, iface+1, strlen(iface)+1);

        /* 跳过 loopback */
        if (strcmp(iface, "lo") == 0) continue;

        if (!first) strcat(result, ",");
        char entry[256];
        snprintf(entry, sizeof(entry),
            "{\"iface\":\"%s\",\"rx\":%llu,\"tx\":%llu}", iface, rx, tx);
        strcat(result, entry);
        first = 0;
    }
    fclose(fp);
    strcat(result, "]");
    strncpy(out, result, size - 1);
    out[size - 1] = '\0';
}

static void get_uptime(char *out, size_t size)
{
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) { snprintf(out, size, "0"); return; }
    char line[64];
    if (fgets(line, sizeof(line), fp)) {
        double uptime;
        sscanf(line, "%lf", &uptime);
        snprintf(out, size, "%.0f", uptime);
    } else {
        snprintf(out, size, "0");
    }
    fclose(fp);
}

/* =========================================================================
 * 请求处理
 * ========================================================================= */

static int handle_api_ask(HttpRequest *req, char *result, size_t size,
                          unsigned long long *latency_ms)
{
    /* 从 body 中提取 prompt */
    char prompt[4096];
    json_get_str(req->body, "prompt", prompt, sizeof(prompt));

    char backend[32], model[64];
    route_request(prompt, backend, model);

    const char *api_key = web_cfg.api_key;
    if (!api_key) api_key = getenv("DEEPSEEK_API_KEY");
    if (!api_key) api_key = getenv("KIMI_API_KEY");
    if (!api_key) {
        snprintf(result, size,
            "{\"error\":\"请设置 DEEPSEEK_API_KEY 或 KIMI_API_KEY 环境变量\"}");
        return -1;
    }

    if (strcmp(backend, "kimi") == 0) {
        return https_post("api.moonshot.cn", 443, "/v1/chat/completions",
                         api_key, model, prompt, result, size, latency_ms);
    } else {
        return https_post("api.deepseek.com", 443, "/v1/chat/completions",
                         api_key, model, prompt, result, size, latency_ms);
    }
}

static void handle_api_status(char *result, size_t size)
{
    char cpu[512], mem[512], load[512], net[4096], uptime[64];
    get_cpu_info(cpu, sizeof(cpu));
    get_mem_info(mem, sizeof(mem));
    get_loadavg(load, sizeof(load));
    get_net_stats(net, sizeof(net));
    get_uptime(uptime, sizeof(uptime));

    snprintf(result, size,
        "{"
        "\"cpu\":%s,"
        "\"memory\":%s,"
        "\"loadavg\":%s,"
        "\"network\":%s,"
        "\"uptime\":%s,"
        "\"backend\":\"%s\","
        "\"version\":\"%s\","
        "\"timestamp\":%lld"
        "}",
        cpu, mem, load, net, uptime,
        web_cfg.backend, VERSION,
        (long long)time(NULL));
}

/* =========================================================================
 * HTML 页面生成
 * ========================================================================= */

static void gen_dashboard_html(char *out, size_t size)
{
    snprintf(out, size,
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>AI Linux Dashboard</title>\n"
"<style>\n"
"* { margin:0; padding:0; box-sizing:border-box; }\n"
"body { font-family: 'SF Mono', 'Cascadia Code', 'JetBrains Mono', monospace;\n"
"       background: #0a0a0f; color: #e0e0e0; min-height:100vh; }\n"
"\n"
"/* 导航 */\n"
".nav { background:#111; border-bottom:1px solid #222;\n"
"       padding:0 24px; display:flex; align-items:center; gap:24px;\n"
"       height:56px; position:sticky; top:0; z-index:100; }\n"
".nav-logo { color:#00d4ff; font-weight:bold; font-size:18px;\n"
"            letter-spacing:-0.5px; }\n"
".nav-logo span { color:#666; font-weight:normal; font-size:12px; margin-left:8px; }\n"
".nav-item { color:#666; text-decoration:none; font-size:14px; padding:0 12px;\n"
"            height:56px; display:flex; align-items:center;\n"
"            border-bottom:2px solid transparent; transition:all 0.2s; cursor:pointer; }\n"
".nav-item:hover { color:#00d4ff; }\n"
".nav-item.active { color:#00d4ff; border-bottom-color:#00d4ff; }\n"
"\n"
"/* 主内容 */\n"
".container { max-width:1400px; margin:0 auto; padding:24px; }\n"
"\n"
"/* 卡片 */\n"
".card { background:#111; border:1px solid #222; border-radius:12px;\n"
"        padding:20px; margin-bottom:16px; }\n"
".card-title { color:#888; font-size:11px; text-transform:uppercase;\n"
"             letter-spacing:1px; margin-bottom:16px; }\n"
".card-value { font-size:32px; font-weight:bold; color:#fff; }\n"
".card-unit { font-size:14px; color:#666; margin-left:4px; }\n"
".card-change { font-size:12px; margin-top:4px; }\n"
".up { color:#4ade80; } .down { color:#f87171; }\n"
"\n"
"/* 指标网格 */\n"
".metrics { display:grid; grid-template-columns:repeat(auto-fit,minmax(200px,1fr)); gap:16px; }\n"
"\n"
"/* AI 对话区 */\n"
".chat-box { background:#111; border:1px solid #222; border-radius:12px;\n"
"           height:400px; display:flex; flex-direction:column; overflow:hidden; }\n"
".chat-header { padding:12px 20px; border-bottom:1px solid #222;\n"
"               display:flex; justify-content:space-between; align-items:center; }\n"
".chat-backend { font-size:12px; color:#00d4ff; background:#0a1a20;\n"
"               padding:4px 12px; border-radius:20px; }\n"
".chat-messages { flex:1; overflow-y:auto; padding:16px; }\n"
".chat-msg { margin-bottom:12px; padding:10px 14px; border-radius:8px; max-width:80%%; }\n"
".chat-msg.user { background:#1a3a4a; margin-left:auto; text-align:right; }\n"
".chat-msg.ai { background:#1a1a2a; }\n"
".chat-msg .role { font-size:10px; color:#666; margin-bottom:4px; }\n"
".chat-msg .text { font-size:14px; line-height:1.6; white-space:pre-wrap; }\n"
".chat-input { padding:12px; border-top:1px solid #222; display:flex; gap:8px; }\n"
".chat-input input { flex:1; background:#0a0a0f; border:1px solid #333;\n"
"                   border-radius:8px; padding:10px 14px; color:#fff;\n"
"                   font-size:14px; font-family:inherit; }\n"
".chat-input input:focus { outline:none; border-color:#00d4ff; }\n"
".chat-input button { background:#00d4ff; color:#000; border:none;\n"
"                    border-radius:8px; padding:10px 20px; font-size:14px;\n"
"                    font-weight:bold; cursor:pointer; font-family:inherit; }\n"
".chat-input button:hover { background:#00b8e6; }\n"
".chat-input button:disabled { background:#333; color:#666; cursor:not-allowed; }\n"
"\n"
"/* 状态网格 */\n"
".status-grid { display:grid; grid-template-columns:1fr 1fr; gap:16px; }\n"
"\n"
"/* 进度条 */\n"
".progress { height:6px; background:#222; border-radius:3px; margin-top:8px;\n"
"           overflow:hidden; }\n"
".progress-bar { height:100%%; border-radius:3px; transition:width 1s; }\n"
".bar-cpu { background:linear-gradient(90deg,#00d4ff,#00ff88); }\n"
".bar-mem { background:linear-gradient(90deg,#a855f7,#ec4899); }\n"
"\n"
"/* 决策日志 */\n"
".log-table { width:100%%; border-collapse:collapse; font-size:13px; }\n"
".log-table th { text-align:left; color:#666; font-size:10px;\n"
"                text-transform:uppercase; padding:8px 12px;\n"
"                border-bottom:1px solid #222; }\n"
".log-table td { padding:10px 12px; border-bottom:1px solid #1a1a1a; }\n"
".log-table tr:hover td { background:#161616; }\n"
".badge { display:inline-block; padding:2px 8px; border-radius:10px;\n"
"         font-size:11px; font-weight:bold; }\n"
".badge-ok { background:#0a2a1a; color:#4ade80; }\n"
".badge-warn { background:#2a1a0a; color:#fb923c; }\n"
".badge-err { background:#2a0a0a; color:#f87171; }\n"
".badge-info { background:#0a1a2a; color:#60a5fa; }\n"
"\n"
"/* Skill 路由 */\n"
".skill-grid { display:grid; grid-template-columns:repeat(3,1fr); gap:12px; }\n"
".skill-item { background:#0a0a0f; border:1px solid #222; border-radius:8px;\n"
"              padding:12px; }\n"
".skill-name { font-size:13px; font-weight:bold; color:#fff; margin-bottom:4px; }\n"
".skill-backend { font-size:11px; color:#00d4ff; }\n"
".skill-weight { display:flex; gap:4px; margin-top:8px; }\n"
".skill-bar { flex:1; height:4px; border-radius:2px; }\n"
".skill-bar-deep { background:#00d4ff; }\n"
".skill-bar-kimi { background:#a855f7; }\n"
"\n"
"/* 实时状态 */\n"
".live-dot { display:inline-block; width:8px; height:8px; border-radius:50%%;\n"
"            background:#4ade80; margin-right:6px; animation:pulse 2s infinite; }\n"
"@keyframes pulse { 0%%,100%% { opacity:1; } 50%% { opacity:0.4; } }\n"
"\n"
"/* 页脚 */\n"
".footer { text-align:center; padding:32px; color:#444; font-size:12px; }\n"
"\n"
"/* 加载动画 */\n"
".typing { display:inline-flex; gap:4px; margin-left:12px; }\n"
".typing span { width:6px; height:6px; background:#666; border-radius:50%%;\n"
"               animation:typing 1.4s infinite; }\n"
".typing span:nth-child(2) { animation-delay:0.2s; }\n"
".typing span:nth-child(3) { animation-delay:0.4s; }\n"
"@keyframes typing { 0%%,100%% { transform:translateY(0); } 50%% { transform:translateY(-6px); } }\n"
"\n"
"/* 快捷命令 */\n"
".cmd-btns { display:flex; gap:8px; margin-bottom:12px; flex-wrap:wrap; }\n"
".cmd-btn { background:#1a1a2a; border:1px solid #333; border-radius:6px;\n"
"           padding:6px 14px; font-size:12px; color:#888; cursor:pointer;\n"
"           font-family:inherit; transition:all 0.2s; }\n"
".cmd-btn:hover { background:#222; color:#00d4ff; border-color:#00d4ff; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"\n"
"<div class=\"nav\">\n"
"  <div class=\"nav-logo\">AI Linux <span>v%s</span></div>\n"
"  <div class=\"nav-item active\" onclick=\"showTab('overview')\">总览</div>\n"
"  <div class=\"nav-item\" onclick=\"showTab('ai')\">AI 问答</div>\n"
"  <div class=\"nav-item\" onclick=\"showTab('sched')\">调度</div>\n"
"  <div class=\"nav-item\" onclick=\"showTab('net')\">网络</div>\n"
"  <div class=\"nav-item\" onclick=\"showTab('security')\">安全</div>\n"
"  <div style=\"flex:1\"></div>\n"
"  <div class=\"nav-item\"><span class=\"live-dot\"></span>实时</div>\n"
"</div>\n"
"\n"
"<div class=\"container\">\n"
"\n"
"<!-- 总览 -->\n"
"<div id=\"tab-overview\">\n"
"  <div style=\"display:grid; grid-template-columns:1fr 2fr; gap:16px; margin-bottom:16px;\">\n"
"\n"
"    <div class=\"card\">\n"
"      <div class=\"card-title\">CPU 使用率</div>\n"
"      <div class=\"card-value\" id=\"cpu-usage\">--<span class=\"card-unit\">%%</span></div>\n"
"      <div class=\"progress\"><div class=\"progress-bar bar-cpu\" id=\"cpu-bar\" style=\"width:0%%\"></div></div>\n"
"      <div class=\"card-change\" id=\"cpu-info\"></div>\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"      <div class=\"card-title\">内存使用率</div>\n"
"      <div class=\"card-value\" id=\"mem-usage\">--<span class=\"card-unit\">%%</span></div>\n"
"      <div class=\"progress\"><div class=\"progress-bar bar-mem\" id=\"mem-bar\" style=\"width:0%%\"></div></div>\n"
"      <div class=\"card-change\" id=\"mem-info\"></div>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class=\"metrics\">\n"
"    <div class=\"card\">\n"
"      <div class=\"card-title\">负载均值</div>\n"
"      <div class=\"card-value\" id=\"loadavg\">--</div>\n"
"      <div class=\"card-change\" id=\"load-procs\">-- 进程</div>\n"
"    </div>\n"
"    <div class=\"card\">\n"
"      <div class=\"card-title\">系统运行时间</div>\n"
"      <div class=\"card-value\" id=\"uptime\">--</div>\n"
"      <div class=\"card-change\">在线</div>\n"
"    </div>\n"
"    <div class=\"card\">\n"
"      <div class=\"card-title\">网络吞吐</div>\n"
"      <div class=\"card-value\" id=\"net-throughput\">--</div>\n"
"      <div class=\"card-change\">接收/发送</div>\n"
"    </div>\n"
"    <div class=\"card\">\n"
"      <div class=\"card-title\">AI 后端</div>\n"
"      <div class=\"card-value\" style=\"font-size:20px\" id=\"backend\">DeepSeek</div>\n"
"      <div class=\"card-change\" id=\"backend-model\">--</div>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- AI 问答 -->\n"
"<div id=\"tab-ai\" style=\"display:none\">\n"
"  <div class=\"card\">\n"
"    <div class=\"card-title\">AI 问答</div>\n"
"    <div class=\"cmd-btns\">\n"
"      <button class=\"cmd-btn\" onclick=\"askPreset('分析当前系统负载，给出优化建议')\">负载分析</button>\n"
"      <button class=\"cmd-btn\" onclick=\"askPreset('检测系统安全威胁')\">安全检测</button>\n"
"      <button class=\"cmd-btn\" onclick=\"askPreset('分析网络连接，找异常')\">网络分析</button>\n"
"      <button class=\"cmd-btn\" onclick=\"askPreset('优化内存使用')\">内存优化</button>\n"
"      <button class=\"cmd-btn\" onclick=\"askPreset('生成一个监控脚本')\">代码生成</button>\n"
"    </div>\n"
"    <div class=\"chat-box\">\n"
"      <div class=\"chat-header\">\n"
"        <span style=\"color:#666;font-size:13px\">AI Linux 助手</span>\n"
"        <span class=\"chat-backend\" id=\"chat-backend\">DeepSeek</span>\n"
"      </div>\n"
"      <div class=\"chat-messages\" id=\"chat-messages\">\n"
"        <div class=\"chat-msg ai\">\n"
"          <div class=\"role\">AI Linux</div>\n"
"          <div class=\"text\">你好！我是 AI Linux 助手。\n"
"我集成了 DeepSeek 和 Kimi K3，可以帮你：\n"
"• 分析系统性能和调度策略\n"
"• 检测网络异常和安全威胁\n"
"• 优化内存管理和 IO 调度\n"
"• 生成系统管理脚本\n\n"
"直接输入你的问题，或点击上方快捷按钮。</div>\n"
"        </div>\n"
"      </div>\n"
"      <div class=\"chat-input\">\n"
"        <input type=\"text\" id=\"chat-input\" placeholder=\"输入你的问题...\" "
"onkeypress=\"if(event.key==='Enter')sendChat()\">\n"
"        <button onclick=\"sendChat()\" id=\"chat-send\">发送</button>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- Skill 路由可视化 -->\n"
"<div id=\"tab-sched\" style=\"display:none\">\n"
"  <div class=\"card\">\n"
"    <div class=\"card-title\">Skill 路由权重</div>\n"
"    <div class=\"skill-grid\">\n"
"      <div class=\"skill-item\">\n"
"        <div class=\"skill-name\">调度 (Scheduling)</div>\n"
"        <div class=\"skill-backend\">DeepSeek 70%% / Kimi 30%%</div>\n"
"        <div class=\"skill-weight\">\n"
"          <div class=\"skill-bar skill-bar-deep\" style=\"flex:70\"></div>\n"
"          <div class=\"skill-bar skill-bar-kimi\" style=\"flex:30\"></div>\n"
"        </div>\n"
"      </div>\n"
"      <div class=\"skill-item\">\n"
"        <div class=\"skill-name\">网络 IO (Network)</div>\n"
"        <div class=\"skill-backend\">DeepSeek 50%% / Kimi 50%%</div>\n"
"        <div class=\"skill-weight\">\n"
"          <div class=\"skill-bar skill-bar-deep\" style=\"flex:50\"></div>\n"
"          <div class=\"skill-bar skill-bar-kimi\" style=\"flex:50\"></div>\n"
"        </div>\n"
"      </div>\n"
"      <div class=\"skill-item\">\n"
"        <div class=\"skill-name\">安全 (Security)</div>\n"
"        <div class=\"skill-backend\">DeepSeek 40%% / Kimi 60%%</div>\n"
"        <div class=\"skill-weight\">\n"
"          <div class=\"skill-bar skill-bar-deep\" style=\"flex:40\"></div>\n"
"          <div class=\"skill-bar skill-bar-kimi\" style=\"flex:60\"></div>\n"
"        </div>\n"
"      </div>\n"
"      <div class=\"skill-item\">\n"
"        <div class=\"skill-name\">内存 (Memory)</div>\n"
"        <div class=\"skill-backend\">DeepSeek 65%% / Kimi 35%%</div>\n"
"        <div class=\"skill-weight\">\n"
"          <div class=\"skill-bar skill-bar-deep\" style=\"flex:65\"></div>\n"
"          <div class=\"skill-bar skill-bar-kimi\" style=\"flex:35\"></div>\n"
"        </div>\n"
"      </div>\n"
"      <div class=\"skill-item\">\n"
"        <div class=\"skill-name\">代码 (Code)</div>\n"
"        <div class=\"skill-backend\">DeepSeek 80%% / Kimi 20%%</div>\n"
"        <div class=\"skill-weight\">\n"
"          <div class=\"skill-bar skill-bar-deep\" style=\"flex:80\"></div>\n"
"          <div class=\"skill-bar skill-bar-kimi\" style=\"flex:20\"></div>\n"
"        </div>\n"
"      </div>\n"
"      <div class=\"skill-item\">\n"
"        <div class=\"skill-name\">编排 (Orchestrate)</div>\n"
"        <div class=\"skill-backend\">DeepSeek 30%% / Kimi 70%%</div>\n"
"        <div class=\"skill-weight\">\n"
"          <div class=\"skill-bar skill-bar-deep\" style=\"flex:30\"></div>\n"
"          <div class=\"skill-bar skill-bar-kimi\" style=\"flex:70\"></div>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class=\"card\">\n"
"    <div class=\"card-title\">AI 决策日志</div>\n"
"    <table class=\"log-table\" id=\"log-table\">\n"
"      <thead>\n"
"        <tr>\n"
"          <th>时间</th>\n"
"          <th>域</th>\n"
"          <th>后端</th>\n"
"          <th>决策</th>\n"
"          <th>置信度</th>\n"
"          <th>延迟</th>\n"
"        </tr>\n"
"      </thead>\n"
"      <tbody id=\"log-body\">\n"
"      </tbody>\n"
"    </table>\n"
"  </div>\n"
"</div>\n"
"\n"
"</div>\n"
"\n"
"<div class=\"footer\">\n"
"  AI Linux | Layer1 (Kernel AI) + Layer2 (Gateway) | DeepSeek + Kimi K3\n"
"</div>\n"
"\n"
"<script>\n"
"let chatHistory = [];\n"
"\n"
"function showTab(tab) {\n"
"  ['overview','ai','sched','net','security'].forEach(t => {\n"
"    document.getElementById('tab-' + t).style.display = t === tab ? 'block' : 'none';\n"
"  });\n"
"  document.querySelectorAll('.nav-item').forEach(el => el.classList.remove('active'));\n"
"  event.target.classList.add('active');\n"
"}\n"
"\n"
"async function refreshStatus() {\n"
"  try {\n"
"    let r = await fetch('/api/status');\n"
"    let d = await r.json();\n"
"\n"
"    // CPU\n"
"    document.getElementById('cpu-usage').innerHTML = d.cpu.usage + '<span class=\"card-unit\">%%</span>';\n"
"    document.getElementById('cpu-bar').style.width = d.cpu.usage + '%%';\n"
"    document.getElementById('cpu-info').textContent =\n"
"      'user: ' + (d.cpu.user/1e7).toFixed(1) + 's  system: ' + (d.cpu.system/1e7).toFixed(1) + 's';\n"
"\n"
"    // Memory\n"
"    document.getElementById('mem-usage').innerHTML = d.memory.usage + '<span class=\"card-unit\">%%</span>';\n"
"    document.getElementById('mem-bar').style.width = d.memory.usage + '%%';\n"
"    document.getElementById('mem-info').textContent =\n"
"      Math.round(d.memory.total/1024) + ' GB 总 | ' + Math.round(d.memory.available/1024) + ' GB 可用';\n"
"\n"
"    // Load\n"
"    let ld = d.loadavg;\n"
"    document.getElementById('loadavg').textContent = ld.load1.toFixed(2);\n"
"    document.getElementById('load-procs').textContent = ld.running + ' / ' + ld.total + ' 进程';\n"
"\n"
"    // Uptime\n"
"    let up = parseFloat(d.uptime);\n"
"    let days = Math.floor(up / 86400);\n"
"    let hrs = Math.floor((up %% 86400) / 3600);\n"
"    let mins = Math.floor((up %% 3600) / 60);\n"
"    document.getElementById('uptime').textContent = days + 'd ' + hrs + 'h ' + mins + 'm';\n"
"\n"
"    // Network\n"
"    let net = d.network;\n"
"    let total_rx = 0, total_tx = 0;\n"
"    for (let iface of net) { total_rx += iface.rx; total_tx += iface.tx; }\n"
"    document.getElementById('net-throughput').textContent =\n"
"      formatBytes(total_rx) + ' / ' + formatBytes(total_tx);\n"
"\n"
"    // Backend\n"
"    document.getElementById('backend').textContent = d.backend === 'kimi' ? 'Kimi K3' : 'DeepSeek';\n"
"    document.getElementById('chat-backend').textContent = d.backend === 'kimi' ? 'Kimi K3' : 'DeepSeek';\n"
"  } catch(e) { console.error('refresh error', e); }\n"
"}\n"
"\n"
"function formatBytes(b) {\n"
"  if (b > 1e12) return (b/1e12).toFixed(1) + ' TB';\n"
"  if (b > 1e9) return (b/1e9).toFixed(1) + ' GB';\n"
"  if (b > 1e6) return (b/1e6).toFixed(1) + ' MB';\n"
"  return (b/1024).toFixed(0) + ' KB';\n"
"}\n"
"\n"
"async function sendChat() {\n"
"  let input = document.getElementById('chat-input');\n"
"  let msg = input.value.trim();\n"
"  if (!msg) return;\n"
"  input.value = '';\n"
"\n"
"  let box = document.getElementById('chat-messages');\n"
"\n"
"  // 用户消息\n"
"  let userDiv = document.createElement('div');\n"
"  userDiv.className = 'chat-msg user';\n"
"  userDiv.innerHTML = '<div class=\"role\">你</div><div class=\"text\">' + escHtml(msg) + '</div>';\n"
"  box.appendChild(userDiv);\n"
"\n"
"  // AI 等待\n"
"  let aiDiv = document.createElement('div');\n"
"  aiDiv.className = 'chat-msg ai';\n"
"  aiDiv.innerHTML = '<div class=\"role\">AI</div><div class=\"text\"><span class=\"typing\"><span></span><span></span><span></span></span></div>';\n"
"  box.appendChild(aiDiv);\n"
"  box.scrollTop = box.scrollHeight;\n"
"\n"
"  let sendBtn = document.getElementById('chat-send');\n"
"  sendBtn.disabled = true;\n"
"\n"
"  try {\n"
"    let resp = await fetch('/api/ask', {\n"
"      method: 'POST',\n"
"      headers: {'Content-Type': 'application/json'},\n"
"      body: JSON.stringify({prompt: msg})\n"
"    });\n"
"    let r = await resp.json();\n"
"\n"
"    if (r.error) {\n"
"      aiDiv.querySelector('.text').textContent = '错误: ' + r.error;\n"
"      aiDiv.querySelector('.text').style.color = '#f87171';\n"
"    } else {\n"
"      aiDiv.querySelector('.text').textContent = r.content || r.answer || JSON.stringify(r);\n"
"    }\n"
"  } catch(e) {\n"
"    aiDiv.querySelector('.text').textContent = '请求失败: ' + e.message;\n"
"    aiDiv.querySelector('.text').style.color = '#f87171';\n"
"  }\n"
"\n"
"  sendBtn.disabled = false;\n"
"  box.scrollTop = box.scrollHeight;\n"
"}\n"
"\n"
"function askPreset(q) {\n"
"  document.getElementById('chat-input').value = q;\n"
"  sendChat();\n"
"}\n"
"\n"
"function escHtml(s) {\n"
"  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');\n"
"}\n"
"\n"
"// 初始加载\n"
"refreshStatus();\n"
"setInterval(refreshStatus, 3000);\n"
"</script>\n"
"</body>\n"
"</html>",
VERSION);
}

/* =========================================================================
 * 路由
 * ========================================================================= */

static void handle_request(int fd, HttpRequest *req)
{
    char path[MAX_PATH];
    strncpy(path, req->path, sizeof(path) - 1);

    /* API */
    if (strcmp(path, "/api/status") == 0) {
        char result[8192];
        handle_api_status(result, sizeof(result));
        send_response(fd, 200, "OK", "application/json", result, strlen(result), 1);
        return;
    }

    if (strcmp(path, "/api/ask") == 0) {
        char result[8192];
        unsigned long long latency;
        int ret = handle_api_ask(req, result, sizeof(result), &latency);

        /* 添加延迟信息到 JSON */
        char final_result[10240];
        if (ret == 0) {
            /* result 已经是 JSON 字符串，找末尾 } 并追加 */
            int len = strlen(result);
            if (len > 0 && result[len-1] == '}') {
                result[len-1] = '\0';
                snprintf(final_result, sizeof(final_result),
                        "%s,\"latency_ms\":%llu}", result, latency);
            } else {
                snprintf(final_result, sizeof(final_result), "%s", result);
            }
        } else {
            snprintf(final_result, sizeof(final_result), "%s", result);
        }

        send_response(fd, 200, "OK", "application/json",
                    final_result, strlen(final_result), 1);
        return;
    }

    if (strcmp(path, "/api/metrics") == 0) {
        char cpu[512], mem[512], load[512], net[4096];
        get_cpu_info(cpu, sizeof(cpu));
        get_mem_info(mem, sizeof(mem));
        get_loadavg(load, sizeof(load));
        get_net_stats(net, sizeof(net));
        char result[8192];
        snprintf(result, sizeof(result),
                "{\"cpu\":%s,\"memory\":%s,\"loadavg\":%s,\"network\":%s}",
                cpu, mem, load, net);
        send_response(fd, 200, "OK", "application/json", result, strlen(result), 1);
        return;
    }

    /* 主页 */
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        char html[204800];
        gen_dashboard_html(html, sizeof(html));
        send_response(fd, 200, "OK", "text/html; charset=utf-8",
                     html, strlen(html), 1);
        return;
    }

    /* 静态资源 */
    if (strncmp(path, "/static/", 8) == 0) {
        send_file(fd, path + 1, "application/octet-stream");
        return;
    }

    send_response(fd, 404, "Not Found", "text/plain", "404 Not Found", 13, 0);
}

/* =========================================================================
 * 连接处理线程
 * ========================================================================= */

static void *conn_handler(void *arg)
{
    int fd = *(int *)arg;
    free(arg);

    char buf[32768];
    int buf_len = 0;

    while (1) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 30);
        if (ret <= 0) break;

        if (pfd.revents & (POLLHUP | POLLERR)) break;

        ssize_t n = recv(fd, buf + buf_len, sizeof(buf) - buf_len - 1, 0);
        if (n <= 0) break;
        buf_len += n;
        buf[buf_len] = '\0';

        /* 检测完整请求 */
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;

            /* 检查 Content-Length */
            const char *cl = strstr(buf, "Content-Length:");
            int content_length = 0;
            if (cl) content_length = atoi(cl + 15);

            int total_len = body_start - buf + content_length;
            if (buf_len >= total_len) {
                HttpRequest req;
                parse_http_request(buf, buf_len, &req);
                handle_request(fd, &req);

                /* HTTP/1.0 或 Connection: close */
                if (strcmp(req.version, "HTTP/1.0") == 0 ||
                    strstr(buf, "Connection: close"))
                    break;

                /* 保持长连接，清空缓冲区 */
                memmove(buf, buf + total_len, buf_len - total_len);
                buf_len -= total_len;
                if (buf_len < 0) break;
            }
        }
    }

    close(fd);
    return NULL;
}

/* =========================================================================
 * 主循环
 * ========================================================================= */

static void accept_loop(int listen_fd)
{
    epoll_fd = epoll_create1(0);

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = listen_fd,
    };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf("  监听 : http://0.0.0.0:%d\n", web_cfg.port);
    printf("  访问 : http://localhost:%d\n", web_cfg.port);
    printf("  按 Ctrl+C 停止\n\n");

    while (web_cfg.running) {
        struct epoll_event events[MAX_EVENTS];
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                /* 新连接 */
                struct sockaddr_in addr;
                socklen_t len = sizeof(addr);
                int client = accept(listen_fd, (struct sockaddr *)&addr, &len);
                if (client >= 0) {
                    set_nonblock(client);
                    int *pfd = malloc(sizeof(int));
                    *pfd = client;
                    pthread_t th;
                    pthread_create(&th, NULL, conn_handler, pfd);
                    pthread_detach(th);
                }
            }
        }
    }
}

/* =========================================================================
 * 信号处理
 * ========================================================================= */

static void signal_handler(int sig)
{
    (void)sig;
    web_cfg.running = 0;
}

/* =========================================================================
 * 使用说明
 * ========================================================================= */

static void usage(const char *prog)
{
    printf("\n");
    printf("  AI Linux Web Dashboard\n");
    printf("  版本: %s\n\n", VERSION);
    printf("  用法:\n");
    printf("    %s [--port PORT] [--key KEY] [--backend BACKEND] [--help]\n\n", prog);
    printf("  选项:\n");
    printf("    --port PORT     监听端口（默认 %d）\n", PORT);
    printf("    --key KEY       API 密钥\n");
    printf("    --key-env VAR   从环境变量读取密钥\n");
    printf("    --backend NAME  后端: deepseek(默认) | kimi\n");
    printf("    --verbose       详细日志\n");
    printf("    --version       显示版本\n");
    printf("    --help          显示帮助\n\n");
    printf("  环境变量:\n");
    printf("    DEEPSEEK_API_KEY / KIMI_API_KEY\n\n");
    printf("  示例:\n");
    printf("    %s --port 8080 --key $DEEPSEEK_API_KEY\n", prog);
    printf("    %s --backend kimi --key $KIMI_API_KEY\n\n", prog);
}

int main(int argc, char **argv)
{
    printf("\n  ╔═══════════════════════════════════════════╗\n");
    printf("  ║  AI Linux — Web Dashboard              ║\n");
    printf("  ║  v%s                              ║\n", VERSION);
    printf("  ╚═══════════════════════════════════════════╝\n\n");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i+1 < argc)
            web_cfg.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--key") == 0 && i+1 < argc)
            web_cfg.api_key = argv[++i];
        else if (strcmp(argv[i], "--key-env") == 0 && i+1 < argc) {
            const char *v = getenv(argv[++i]);
            if (v) web_cfg.api_key = v;
        }
        else if (strcmp(argv[i], "--backend") == 0 && i+1 < argc)
            web_cfg.backend = argv[++i];
        else if (strcmp(argv[i], "--verbose") == 0)
            web_cfg.verbose = 1;
        else if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", VERSION); return 0;
        }
        else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        }
    }

    if (!web_cfg.api_key) {
        const char *k = getenv("DEEPSEEK_API_KEY");
        if (!k) k = getenv("KIMI_API_KEY");
        if (k) web_cfg.api_key = k;
    }

    if (!web_cfg.api_key) {
        fprintf(stderr, "错误: 请设置 DEEPSEEK_API_KEY 或 KIMI_API_KEY 环境变量\n");
        fprintf(stderr, "   或使用 --key 参数\n\n");
        usage(argv[0]);
        return 1;
    }

    printf("  后端:   %s\n", web_cfg.backend);
    printf("  API Key: ***（已设置）\n\n");

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    int listen_fd = create_listen_socket(web_cfg.port);
    if (listen_fd < 0) {
        fprintf(stderr, "错误: 无法监听端口 %d\n", web_cfg.port);
        return 1;
    }

    accept_loop(listen_fd);
    close(listen_fd);

    printf("\n  再见！\n\n");
    return 0;
}
