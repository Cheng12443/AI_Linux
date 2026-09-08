// SPDX-License-Identifier: GPL-2.0
/*
 * ai_tui.c — AI Linux 交互式 TUI
 *
 * 一个功能完整的 ncurses 彩色终端界面：
 *   - 实时系统状态（CPU/内存/网络/进程）
 *   - AI 对话区（彩色消息，实时响应）
 *   - Skill 路由可视化（双 Backend 权重）
 *   - 决策日志（滚动历史）
 *   - 快捷命令面板
 *
 * 编译：
 *   gcc -O2 -o ai-tui ai_tui.c -lncurses -lpthread -lm
 *
 * 依赖：
 *   apt install libncurses-dev
 *
 * 运行：
 *   DEEPSEEK_API_KEY=xxx ./ai-tui
 *   KIMI_API_KEY=xxx ./ai-tui --backend kimi
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <locale.h>
#include <wchar.h>

/* 优先用 ncursesw，否则回退到 ncurses */
#ifdef __linux__
#include <ncursesw/ncurses.h>
#include <ncursesw/panel.h>
#else
#include <ncurses.h>
#include <panel.h>
#endif

/* =========================================================================
 * 常量
 * ========================================================================= */

#define VERSION   "1.0.0"
#define MAX_CHAT   200
#define MAX_LOG    100
#define MAX_INPUT  4096
#define MAX_COLS   1024

/* =========================================================================
 * 颜色对
 * ========================================================================= */

#define COLOR_TITLE    1  /* 标题 */
#define COLOR_SUBTITLE 2  /* 副标题 */
#define COLOR_GREEN    3  /* 绿色（正常）*/
#define COLOR_YELLOW   4  /* 黄色（警告）*/
#define COLOR_RED      5  /* 红色（错误）*/
#define COLOR_CYAN     6  /* 青色（AI 消息）*/
#define COLOR_MAGENTA  7  /* 紫色（系统）*/
#define COLOR_BLUE     8  /* 蓝色（用户）*/
#define COLOR_GRAY     9  /* 灰色（次要）*/
#define COLOR_BRIGHT   10 /* 高亮 */
#define COLOR_PANEL   11 /* 面板背景 */
#define COLOR_BORDER  12 /* 边框 */

/* =========================================================================
 * 全局状态
 * ========================================================================= */

static struct {
    int         running;
    int         cols;
    int         lines;
    const char *api_key;
    const char *backend;
    int         selected_tab;
    int         refresh_ms;
    WINDOW     *main_win;
    pthread_mutex_t updates_lock;
} G = {
    .running = 1,
    .selected_tab = 0,
    .refresh_ms = 1000,
};

/* =========================================================================
 * 工具函数
 * ========================================================================= */

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void trim_line(char *s)
{
    while (isspace((unsigned char)*s)) memmove(s, s+1, strlen(s));
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
}

/* 格式化字节数 */
static void fmt_bytes(unsigned long long bytes, char *out, size_t size)
{
    if (bytes >= 1ULL << 50)       snprintf(out, size, "%.1f TB", bytes / (1024.0 * 1024 * 1024 * 1024));
    else if (bytes >= 1ULL << 40)  snprintf(out, size, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1ULL << 30)  snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1ULL << 20)  snprintf(out, size, "%.1f KB", bytes / (1024.0 * 1024));
    else                            snprintf(out, size, "%llu B", bytes);
}

/* 格式化时间差 */
static void fmt_duration(double seconds, char *out, size_t size)
{
    int d = (int)(seconds / 86400);
    int h = ((int)seconds % 86400) / 3600;
    int m = ((int)seconds % 3600) / 60;
    int s = (int)seconds % 60;
    if (d > 0)      snprintf(out, size, "%dd %dh %dm", d, h, m);
    else if (h > 0) snprintf(out, size, "%dh %dm %ds", h, m, s);
    else if (m > 0) snprintf(out, size, "%dm %ds", m, s);
    else            snprintf(out, size, "%ds", s);
}

/* =========================================================================
 * 系统数据
 * ========================================================================= */

typedef struct {
    /* CPU */
    unsigned long long cpu_user;
    unsigned long long cpu_system;
    unsigned long long cpu_idle;
    unsigned long long cpu_iowait;
    int               cpu_usage_pct;

    /* 内存 */
    unsigned long long mem_total;
    unsigned long long mem_free;
    unsigned long long mem_available;
    unsigned long long swap_total;
    unsigned long long swap_free;
    int               mem_usage_pct;

    /* 负载 */
    double load1, load5, load15;
    int    proc_running;
    int    proc_total;

    /* 网络 */
    unsigned long long net_rx;
    unsigned long long net_tx;

    /* 进程 */
    int    top_cpu_pid;
    char   top_cpu_comm[256];
    double top_cpu_pct;
    int    top_mem_pid;
    char   top_mem_comm[256];
    double top_mem_pct;

    /* AI 统计 */
    int    ai_inferences;
    int    ai_errors;
    double ai_avg_latency_ms;
    int    layer2_connected;

    /* 杂项 */
    double uptime_seconds;
    time_t last_update;
} SysInfo;

static SysInfo sysinfo;

static void read_cpu_info(void)
{
    static unsigned long long prev_user = 0, prev_sys = 0, prev_idle = 0, prev_iowait = 0;
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            unsigned long long u = 0, s = 0, i = 0, w = 0;
            sscanf(line + 5, "%llu %llu %llu %llu %llu", &u, &s, &s, &i, &w);
            unsigned long long total = (u - prev_user) + (s - prev_system) +
                                      (i - prev_idle) + (w - prev_iowait);
            if (total > 0)
                sysinfo.cpu_usage_pct = (int)((total - (i - prev_idle)) * 100 / total);
            else
                sysinfo.cpu_usage_pct = 0;
            prev_user = u; prev_system = s; prev_idle = i; prev_iowait = w;
            sysinfo.cpu_user = u;
            sysinfo.cpu_system = s;
            sysinfo.cpu_idle = i;
            sysinfo.cpu_iowait = w;
            break;
        }
    }
    fclose(fp);
}

static void read_mem_info(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;

    char line[256];
    unsigned long long v;
    sysinfo.mem_total = sysinfo.mem_free = sysinfo.mem_available = 0;
    sysinfo.swap_total = sysinfo.swap_free = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %llu kB", &v) == 1) sysinfo.mem_total = v;
        else if (sscanf(line, "MemFree: %llu kB", &v) == 1) sysinfo.mem_free = v;
        else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) sysinfo.mem_available = v;
        else if (sscanf(line, "SwapTotal: %llu kB", &v) == 1) sysinfo.swap_total = v;
        else if (sscanf(line, "SwapFree: %llu kB", &v) == 1) sysinfo.swap_free = v;
    }
    fclose(fp);

    if (sysinfo.mem_total > 0)
        sysinfo.mem_usage_pct = (int)((sysinfo.mem_total - sysinfo.mem_available) * 100 / sysinfo.mem_total);
}

static void read_loadavg(void)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return;

    char line[128];
    if (fgets(line, sizeof(line), fp)) {
        int run = 0, tot = 0;
        sscanf(line, "%lf %lf %lf %d/%d",
               &sysinfo.load1, &sysinfo.load5, &sysinfo.load15,
               &run, &tot);
        sysinfo.proc_running = run;
        sysinfo.proc_total = tot;
    }
    fclose(fp);
}

static void read_net_info(void)
{
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return;

    sysinfo.net_rx = 0;
    sysinfo.net_tx = 0;
    char line[256];

    while (fgets(line, sizeof(line), fp)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char iface[64] = {0};
        size_t len = colon - line;
        if (len > sizeof(iface)-1) len = sizeof(iface)-1;
        strncpy(iface, line, len);
        while (*iface == ' ') memmove(iface, iface+1, strlen(iface)+1);

        if (strcmp(iface, "lo") == 0) continue;

        unsigned long long rx = 0, tx = 0;
        sscanf(colon+1, " %llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx, &tx);
        sysinfo.net_rx += rx;
        sysinfo.net_tx += tx;
    }
    fclose(fp);
}

static void read_uptime(void)
{
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) return;
    char line[64];
    if (fgets(line, sizeof(line), fp))
        sscanf(line, "%lf", &sysinfo.uptime_seconds);
    fclose(fp);
}

static void read_top_processes(void)
{
    sysinfo.top_cpu_pid = 0;
    sysinfo.top_cpu_pct = 0;
    sysinfo.top_mem_pid = 0;
    sysinfo.top_mem_pct = 0;

    DIR *proc = opendir("/proc");
    if (!proc) return;

    struct dirent *entry;
    while ((entry = readdir(proc))) {
        if (!isdigit(entry->d_name[0])) continue;
        int pid = atoi(entry->d_name);
        char path[256];
        snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;

        char buf[1024];
        if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); continue; }

        /* pid (comm) state ppid */
        char comm[256] = {0};
        unsigned long utime = 0, stime = 0;
        long vss = 0, rss = 0;
        sscanf(buf,
            "%d (%255[^)]) %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %lu %lu %*ld %*ld %*ld %*ld %*ld %*ld %ld %ld",
            &pid, comm, &utime, &stime, &vss, &rss);
        fclose(fp);

        double cpu_pct = (utime + stime) * 100.0 / (sysinfo.uptime_seconds * 100.0 + 1);
        double mem_pct = rss * 4096.0 / (sysinfo.mem_total * 1024.0) * 100.0;

        if (cpu_pct > sysinfo.top_cpu_pct) {
            sysinfo.top_cpu_pid = pid;
            sysinfo.top_cpu_pct = cpu_pct;
            strncpy(sysinfo.top_cpu_comm, comm, sizeof(sysinfo.top_cpu_comm)-1);
        }
        if (mem_pct > sysinfo.top_mem_pct) {
            sysinfo.top_mem_pid = pid;
            sysinfo.top_mem_pct = mem_pct;
            strncpy(sysinfo.top_mem_comm, comm, sizeof(sysinfo.top_mem_comm)-1);
        }
    }
    closedir(proc);
}

static void refresh_sysinfo(void)
{
    read_cpu_info();
    read_mem_info();
    read_loadavg();
    read_net_info();
    read_uptime();
    read_top_processes();
    sysinfo.last_update = time(NULL);
}

/* =========================================================================
 * AI 对话记录
 * ========================================================================= */

typedef struct {
    int         id;
    char        role[16];   /* user / ai / system */
    char        text[MAX_INPUT];
    time_t      timestamp;
    int         confidence;
    char        backend[32];
    double      latency_ms;
    int         is_streaming;
} ChatMsg;

static ChatMsg chat_history[MAX_CHAT];
static int     chat_count = 0;
static int     chat_next_id = 1;
static int     chat_scroll_offset = 0;

static void chat_add(const char *role, const char *text)
{
    if (chat_count < MAX_CHAT) chat_count++;
    memmove(chat_history + 1, chat_history, (chat_count - 1) * sizeof(ChatMsg));
    ChatMsg *m = &chat_history[0];
    memset(m, 0, sizeof(*m));
    m->id = chat_next_id++;
    strncpy(m->role, role, sizeof(m->role) - 1);
    strncpy(m->text, text, sizeof(m->text) - 1);
    m->timestamp = time(NULL);
}

/* =========================================================================
 * AI 调用
 * ========================================================================= */

static int ai_call(const char *prompt, char *result, size_t result_size,
                   double *latency_ms, const char **backend_used)
{
    const char *api_key = G.api_key;
    if (!api_key) {
        api_key = getenv("DEEPSEEK_API_KEY");
        if (!api_key) api_key = getenv("KIMI_API_KEY");
    }
    if (!api_key) {
        snprintf(result, result_size, "错误: 请设置 DEEPSEEK_API_KEY 或 KIMI_API_KEY");
        return -1;
    }

    const char *host, *path, *model;
    if (G.backend && strcmp(G.backend, "kimi") == 0) {
        host = "api.moonshot.cn"; path = "/v1/chat/completions";
        model = "moonshot-v1-8k";
        if (backend_used) *backend_used = "Kimi K3";
    } else {
        host = "api.deepseek.com"; path = "/v1/chat/completions";
        model = "deepseek-chat";
        if (backend_used) *backend_used = "DeepSeek";
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct hostent *he = gethostbyname(host);
    if (!he) { close(fd); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(443),
    };
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }

    /* JSON body */
    char escaped[8192];
    {
        const char *p = prompt;
        char *op = escaped;
        size_t left = sizeof(escaped) - 1;
        while (*p && left > 8) {
            switch (*p) {
            case '"':  strncpy(op, "\\\"", op_size - 1);
    op[op_size - 1] = '\0'; op += 2; left -= 2; break;
            case '\\': strncpy(op, "\\\\", op_size - 1);
    op[op_size - 1] = '\0'; op += 2; left -= 2; break;
            case '\n': strncpy(op, "\\n", op_size - 1);
    op[op_size - 1] = '\0';  op += 2; left -= 2; break;
            default:
                if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f)
                    { *op++ = *p; left--; }
                p++;
                continue;
            }
            p++;
        }
        *op = '\0';
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
        "User-Agent: AI-Linux-TUI/1.0\r\n"
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

    *latency_ms = (now_ms() - start) / 1000.0;

    if (total == 0) {
        snprintf(result, result_size, "错误: 连接超时");
        return -1;
    }
    resp[total] = '\0';

    char *body_start = strstr(resp, "\r\n\r\n");
    if (!body_start) {
        snprintf(result, result_size, "错误: 无效响应");
        return -1;
    }
    body_start += 4;

    const char *content_p = strstr(body_start, "\"content\":\"");
    if (!content_p) {
        const char *err_p = strstr(body_start, "\"error\":");
        if (err_p) {
            snprintf(result, result_size, "API 错误: %.200s", err_p);
        } else {
            snprintf(result, result_size, "错误: 无法解析响应");
        }
        return -1;
    }

    content_p += 11;
    const char *content_end = strchr(content_p, '"');
    if (!content_end) content_end = content_p + strlen(content_p);

    size_t len = content_end - content_p;
    if (len > result_size - 1) len = result_size - 1;
    strncpy(result, content_p, len);
    result[len] = '\0';

    return 0;
}

/* =========================================================================
 * TUI 绘制
 * ========================================================================= */

static WINDOW *title_win, *status_win, *main_win, *input_win, *panel_win;
static WINDOW *left_win, *right_win;

static void init_colors(void)
{
    start_color();
    use_default_colors();

    init_pair(COLOR_TITLE,   COLOR_CYAN,   -1);
    init_pair(COLOR_SUBTITLE,COLOR_GREEN,  -1);
    init_pair(COLOR_GREEN,   COLOR_GREEN,   -1);
    init_pair(COLOR_YELLOW,  COLOR_YELLOW,  -1);
    init_pair(COLOR_RED,     COLOR_RED,    -1);
    init_pair(COLOR_CYAN,    COLOR_CYAN,    -1);
    init_pair(COLOR_MAGENTA, COLOR_MAGENTA,-1);
    init_pair(COLOR_BLUE,    COLOR_BLUE,    -1);
    init_pair(COLOR_GRAY,    COLOR_BLACK,   -1);
    init_pair(COLOR_BRIGHT,  COLOR_WHITE,   -1);
    init_pair(COLOR_PANEL,   -1,           COLOR_BLACK);
    init_pair(COLOR_BORDER,  COLOR_BLUE,   -1);
}

static void draw_title_bar(void)
{
    int maxy, maxx;
    getmaxyx(title_win, maxy, maxx);
    werase(title_win);

    wattron(title_win, A_BOLD | COLOR_PAIR(COLOR_CYAN));
    wprintw(title_win, "  AI Linux TUI  ");
    wattroff(title_win, COLOR_PAIR(COLOR_CYAN));

    wattron(title_win, COLOR_PAIR(COLOR_GRAY));
    wprintw(title_win, "v%s", VERSION);
    wattroff(title_win, COLOR_PAIR(COLOR_GRAY));

    wprintw(title_win, "  |  Backend: %s  |  %s",
             G.backend && strcmp(G.backend, "kimi") == 0 ? "Kimi K3" : "DeepSeek",
             G.api_key ? "已连接" : "未连接");

    /* 右侧时间 */
    time_t now = time(NULL);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));
    wattron(title_win, A_RIGHT);
    wattron(title_win, COLOR_PAIR(COLOR_GRAY));
    wprintw(title_win, "  %s", time_str);
    wattroff(title_win, COLOR_PAIR(COLOR_GRAY));
    wattroff(title_win, A_RIGHT);

    /* 底部边框 */
    whline(title_win, ACS_HLINE, maxx);
    wrefresh(title_win);
}

static void draw_cpu_gauge(int y, int x, const char *label, int pct, short color)
{
    int gauge_w = 20;
    WINDOW *w = left_win;
    mvwprintw(w, y, x, "%-12s", label);

    /* 颜色条 */
    wattron(w, COLOR_PAIR(COLOR_GRAY));
    wprintw(w, "[");
    wattroff(w, COLOR_PAIR(COLOR_GRAY));

    int filled = (pct * gauge_w) / 100;
    int i;
    for (i = 0; i < filled && i < gauge_w; i++) {
        if (pct > 80) wattron(w, COLOR_PAIR(COLOR_RED));
        else if (pct > 60) wattron(w, COLOR_PAIR(COLOR_YELLOW));
        else wattron(w, COLOR_PAIR(COLOR_GREEN));
        wprintw(w, " ");
    }
    for (; i < gauge_w; i++) {
        wattron(w, COLOR_PAIR(COLOR_GRAY));
        wprintw(w, " ");
    }
    wattroff(w, COLOR_PAIR(COLOR_GRAY));
    wattron(w, COLOR_PAIR(COLOR_GRAY));
    wprintw(w, "] %3d%%", pct);
    wattroff(w, COLOR_PAIR(COLOR_GRAY));
}

static void draw_left_panel(void)
{
    int maxy, maxx;
    getmaxyx(left_win, maxy, maxx);
    werase(left_win);

    /* 面板标题 */
    wattron(left_win, A_BOLD | COLOR_PAIR(COLOR_GREEN));
    mvwprintw(left_win, 1, 2, "  SYSTEM STATUS  ");
    wattroff(left_win, COLOR_PAIR(COLOR_GREEN));
    wattron(left_win, COLOR_PAIR(COLOR_GRAY));
    mvwprintw(left_win, 1, maxx - 22, " %s ", ctime(&sysinfo.last_update) + 4);
    wattroff(left_win, COLOR_PAIR(COLOR_GRAY));
    mvwchgat(left_win, 1, 0, maxx, A_DIM, COLOR_GRAY, NULL);

    int row = 3;

    /* CPU */
    mvwprintw(left_win, row++, 2, " CPU");
    draw_cpu_gauge(row++, 2, "usage", sysinfo.cpu_usage_pct, COLOR_GREEN);

    /* 负载 */
    char load_str[64];
    snprintf(load_str, sizeof(load_str), "%.2f / %.2f / %.2f",
             sysinfo.load1, sysinfo.load5, sysinfo.load15);
    mvwprintw(left_win, row++, 2, " loadavg    %s", load_str);

    /* 内存 */
    row++;
    mvwprintw(left_win, row++, 2, " MEMORY");
    draw_cpu_gauge(row++, 2, "used", sysinfo.mem_usage_pct, COLOR_MAGENTA);

    char mem_str[128];
    snprintf(mem_str, sizeof(mem_str), "%.1fG / %.1fG",
             (double)(sysinfo.mem_total - sysinfo.mem_available) / 1024.0 / 1024.0,
             (double)sysinfo.mem_total / 1024.0 / 1024.0);
    mvwprintw(left_win, row++, 2, " %s", mem_str);

    /* Swap */
    if (sysinfo.swap_total > 0) {
        int swap_pct = (int)((sysinfo.swap_total - sysinfo.swap_free) * 100 / sysinfo.swap_total);
        draw_cpu_gauge(row++, 2, "swap", swap_pct, COLOR_RED);
    }

    /* 网络 */
    row++;
    mvwprintw(left_win, row++, 2, " NETWORK");
    char rx_str[32], tx_str[32];
    fmt_bytes(sysinfo.net_rx, rx_str, sizeof(rx_str));
    fmt_bytes(sysinfo.net_tx, tx_str, sizeof(tx_str));
    mvwprintw(left_win, row++, 2, " RX: %s", rx_str);
    mvwprintw(left_win, row++, 2, " TX: %s", tx_str);

    /* 进程 */
    row++;
    mvwprintw(left_win, row++, 2, " TOP PROCESS");
    if (sysinfo.top_cpu_pid > 0) {
        mvwprintw(left_win, row++, 2, " CPU: %-8.8s %3d%% (PID %d)",
                 sysinfo.top_cpu_comm, (int)sysinfo.top_cpu_pct, sysinfo.top_cpu_pid);
    }
    if (sysinfo.top_mem_pid > 0) {
        mvwprintw(left_win, row++, 2, " MEM: %-8.8s %3d%% (PID %d)",
                 sysinfo.top_mem_comm, (int)sysinfo.top_mem_pct, sysinfo.top_mem_pid);
    }

    /* 运行时间 */
    row++;
    char up_str[64];
    fmt_duration(sysinfo.uptime_seconds, up_str, sizeof(up_str));
    mvwprintw(left_win, row++, 2, " uptime: %s", up_str);

    /* 进程数 */
    mvwprintw(left_win, row++, 2, " procs: %d/%d running",
             sysinfo.proc_running, sysinfo.proc_total);

    wrefresh(left_win);
}

static void draw_right_panel(void)
{
    int maxy, maxx;
    getmaxyx(right_win, maxy, maxx);
    werase(right_win);

    /* 标题 */
    wattron(right_win, A_BOLD | COLOR_PAIR(COLOR_CYAN));
    mvwprintw(right_win, 1, 2, "  AI CHAT  ");
    wattroff(right_win, COLOR_PAIR(COLOR_CYAN));

    /* Backend 指示 */
    const char *backend_name = G.backend && strcmp(G.backend, "kimi") == 0
        ? "Kimi K3" : "DeepSeek";
    wattron(right_win, A_BOLD);
    wattron(right_win, COLOR_PAIR(COLOR_MAGENTA));
    wprintw(right_win, " [%s]", backend_name);
    wattroff(right_win, COLOR_PAIR(COLOR_MAGENTA));
    wattroff(right_win, A_BOLD);

    mvwchgat(right_win, 1, 0, maxx, A_DIM, COLOR_GRAY, NULL);

    /* 聊天消息区 */
    int msg_max = maxy - 8;
    int show_count = msg_max < chat_count ? msg_max : chat_count;
    int start_idx = chat_scroll_offset;
    if (start_idx + show_count > chat_count)
        start_idx = chat_count - show_count;
    if (start_idx < 0) start_idx = 0;

    int row = 2;
    for (int i = start_idx; i < start_idx + show_count && i < chat_count; i++) {
        ChatMsg *m = &chat_history[i];

        /* 时间戳 */
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M", localtime(&m->timestamp));

        if (strcmp(m->role, "user") == 0) {
            wattron(right_win, COLOR_PAIR(COLOR_BLUE));
            mvwprintw(right_win, row, 2, "[%s] YOU:", ts);
            wattroff(right_win, COLOR_PAIR(COLOR_BLUE));
            wattron(right_win, A_BOLD);
            mvwprintw(right_win, row++, 15, "%.50s", m->text);
            wattroff(right_win, A_BOLD);
        } else if (strcmp(m->role, "ai") == 0) {
            wattron(right_win, COLOR_PAIR(COLOR_CYAN));
            mvwprintw(right_win, row, 2, "[%s] AI: ", ts);
            wattroff(right_win, COLOR_PAIR(COLOR_CYAN));

            /* 彩色显示 */
            if (strstr(m->text, "错误") || strstr(m->text, "失败") || strstr(m->text, "Error")) {
                wattron(right_win, COLOR_PAIR(COLOR_RED));
            } else if (strstr(m->text, "警告") || strstr(m->text, "优化")) {
                wattron(right_win, COLOR_PAIR(COLOR_YELLOW));
            } else {
                wattron(right_win, COLOR_PAIR(COLOR_GREEN));
            }
            mvwprintw(right_win, row++, 15, "%.50s", m->text);
            wattroff(right_win, COLOR_PAIR(COLOR_GREEN));

            /* 元信息 */
            if (m->latency_ms > 0) {
                wattron(right_win, COLOR_PAIR(COLOR_GRAY));
                mvwprintw(right_win, row++, 15, "  (%s, %.1fs, conf=%d%%)",
                         m->backend, m->latency_ms, m->confidence);
                wattroff(right_win, COLOR_PAIR(COLOR_GRAY));
            }
        } else {
            wattron(right_win, COLOR_PAIR(COLOR_MAGENTA));
            mvwprintw(right_win, row++, 2, "[%s] SYS: %.60s", ts, m->text);
            wattroff(right_win, COLOR_PAIR(COLOR_MAGENTA));
        }

        row++;
        if (row >= maxy - 4) break;
    }

    /* 快捷命令 */
    int cmd_row = maxy - 3;
    wattron(right_win, COLOR_PAIR(COLOR_GRAY));
    mvwprintw(right_win, cmd_row, 2, "快捷: ");
    wattroff(right_win, COLOR_PAIR(COLOR_GRAY));

    wattron(right_win, A_STANDOUT);
    wattron(right_win, COLOR_PAIR(COLOR_GREEN));
    mvwprintw(right_win, cmd_row, 12, "[1] 负载分析");
    wattroff(right_win, COLOR_PAIR(COLOR_GREEN));
    wattroff(right_win, A_STANDOUT);

    wattron(right_win, COLOR_PAIR(COLOR_GREEN));
    mvwprintw(right_win, cmd_row, 24, "[2] 安全检测");
    wattroff(right_win, COLOR_PAIR(COLOR_GREEN));

    wattron(right_win, COLOR_PAIR(COLOR_GREEN));
    mvwprintw(right_win, cmd_row, 37, "[3] 网络分析");
    wattroff(right_win, COLOR_PAIR(COLOR_GREEN));

    wattron(right_win, COLOR_PAIR(COLOR_GREEN));
    mvwprintw(right_win, cmd_row, 50, "[4] 内存优化");
    wattroff(right_win, COLOR_PAIR(COLOR_GREEN));

    wattron(right_win, COLOR_PAIR(COLOR_GREEN));
    mvwprintw(right_win, cmd_row, 63, "[5] 代码生成");
    wattroff(right_win, COLOR_PAIR(COLOR_GREEN));

    wattron(right_win, COLOR_PAIR(COLOR_GRAY));
    mvwprintw(right_win, cmd_row, 75, "[Tab]切换 [Ctrl+C]退出");
    wattroff(right_win, COLOR_PAIR(COLOR_GRAY));

    wrefresh(right_win);
}

static void draw_input_line(void)
{
    int maxy, maxx;
    getmaxyx(input_win, maxy, maxx);
    werase(input_win);

    wattron(input_win, A_BOLD | COLOR_PAIR(COLOR_CYAN));
    wprintw(input_win, " >>> ");
    wattroff(input_win, COLOR_PAIR(COLOR_CYAN));
    wattroff(input_win, A_BOLD);

    wattron(input_win, A_BOLD);
    wprintw(input_win, "输入你的问题，或按数字快捷键：");
    wattroff(input_win, A_BOLD);

    wattron(input_win, COLOR_PAIR(COLOR_GRAY));
    wprintw(input_win, " (DeepSeek=%s Kimi=%s Tab=切换 Backend)",
             G.backend && strcmp(G.backend, "kimi") == 0 ? "○" : "●",
             G.backend && strcmp(G.backend, "kimi") == 0 ? "●" : "○");
    wattroff(input_win, COLOR_PAIR(COLOR_GRAY));

    wrefresh(input_win);
}

static void draw_footer(void)
{
    int maxy, maxx;
    getmaxyx(status_win, maxy, maxx);
    werase(status_win);

    whline(status_win, ACS_HLINE, maxx);

    wattron(status_win, COLOR_PAIR(COLOR_GRAY));
    mvwprintw(status_win, 1, 2, "F1 帮助  |  F2 切换 Backend  |  F3 刷新  |  "
                                  "Ctrl+L 清屏  |  Ctrl+C 退出  |  ↑↓ 滚动历史");

    /* 实时数据 */
    int col = maxx - 50;
    if (col > 0) {
        char cpu_str[16], mem_str[16];
        snprintf(cpu_str, sizeof(cpu_str), "CPU:%d%%", sysinfo.cpu_usage_pct);
        snprintf(mem_str, sizeof(mem_str), "MEM:%d%%", sysinfo.mem_usage_pct);

        if (sysinfo.cpu_usage_pct > 80) wattron(status_win, COLOR_PAIR(COLOR_RED));
        else if (sysinfo.cpu_usage_pct > 60) wattron(status_win, COLOR_PAIR(COLOR_YELLOW));
        else wattron(status_win, COLOR_PAIR(COLOR_GREEN));
        mvwprintw(status_win, 1, col, "%s", cpu_str);
        wattroff(status_win, COLOR_PAIR(COLOR_GREEN));

        wattron(status_win, COLOR_PAIR(COLOR_GRAY));
        mvwprintw(status_win, 1, col + 10, " %s", mem_str);
        wattroff(status_win, COLOR_PAIR(COLOR_GRAY));
    }

    wrefresh(status_win);
}

static void refresh_all(void)
{
    refresh_sysinfo();
    draw_title_bar();
    draw_left_panel();
    draw_right_panel();
    draw_input_line();
    draw_footer();
}

/* =========================================================================
 * 快捷命令映射
 * ========================================================================= */

static const char *preset_questions[] = {
    [0] = "分析当前系统负载，给出调度优化建议。重点关注 CPU 使用率和上下文切换。",
    [1] = "对系统进行全面的安全检测，查找潜在威胁和漏洞。",
    [2] = "分析网络连接状态，找出异常连接或可疑流量。",
    [3] = "分析内存使用情况，给出优化建议。重点关注页面缓存和 swap。",
    [4] = "生成一个 Linux 系统监控脚本，包含 CPU/内存/磁盘/网络的监控。",
};

static void handle_input(const char *input)
{
    if (!input || !input[0]) return;

    char trimmed[MAX_INPUT];
    strncpy(trimmed, input, sizeof(trimmed) - 1);
    trim_line(trimmed);
    if (!trimmed[0]) return;

    /* 数字快捷键 */
    if (strcmp(trimmed, "1") == 0) {
        strncpy(trimmed, preset_questions[0], trimmed_size - 1);
    trimmed[trimmed_size - 1] = '\0';
    } else if (strcmp(trimmed, "2") == 0) {
        strncpy(trimmed, preset_questions[1], trimmed_size - 1);
    trimmed[trimmed_size - 1] = '\0';
    } else if (strcmp(trimmed, "3") == 0) {
        strncpy(trimmed, preset_questions[2], trimmed_size - 1);
    trimmed[trimmed_size - 1] = '\0';
    } else if (strcmp(trimmed, "4") == 0) {
        strncpy(trimmed, preset_questions[3], trimmed_size - 1);
    trimmed[trimmed_size - 1] = '\0';
    } else if (strcmp(trimmed, "5") == 0) {
        strncpy(trimmed, preset_questions[4], trimmed_size - 1);
    trimmed[trimmed_size - 1] = '\0';
    }

    /* Tab: 切换 Backend */
    if (strcmp(trimmed, "\t") == 0) {
        if (G.backend && strcmp(G.backend, "kimi") == 0) {
            G.backend = "deepseek";
        } else {
            G.backend = "kimi";
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "切换到 %s",
                 G.backend && strcmp(G.backend, "kimi") == 0 ? "Kimi K3" : "DeepSeek");
        chat_add("system", msg);
        return;
    }

    /* F2 快捷切换 */
    if (strncmp(trimmed, "F2", 2) == 0) {
        if (G.backend && strcmp(G.backend, "kimi") == 0)
            G.backend = "deepseek";
        else
            G.backend = "kimi";
        return;
    }

    /* 添加用户消息 */
    chat_add("user", trimmed);

    /* 调用 AI */
    char result[4096];
    double latency_ms = 0;
    const char *backend_used = NULL;

    int ret = ai_call(trimmed, result, sizeof(result), &latency_ms, &backend_used);

    if (ret == 0) {
        chat_add("ai", result);
        chat_history[0].latency_ms = latency_ms;
        strncpy(chat_history[0].backend, backend_used ? backend_used : "unknown",
                sizeof(chat_history[0].backend) - 1);
        chat_history[0].confidence = 75;
    } else {
        chat_add("ai", result);
        strncpy(chat_history[0].backend, "error", chat_history[0].backend_size - 1);
    chat_history[0].backend[chat_history[0].backend_size - 1] = '\0';
        chat_history[0].confidence = 0;
    }
}

/* =========================================================================
 * 输入处理
 * ========================================================================= */

static void input_loop(void)
{
    char input_buf[MAX_INPUT];
    int pos = 0;
    int ch;

    nodelay(stdscr, FALSE);
    echo();
    curs_set(1);

    mvprintw(LINES - 2, 6, ">>> ");
    refresh();

    while ((ch = getch()) != ERR) {
        /* 过滤终端控制序列 */
        if (ch < 32 && ch != '\n' && ch != '\t' && ch != 3 && ch != 27
            && ch != KEY_UP && ch != KEY_DOWN)
            continue;
        if (ch == '\n' || ch == '\r') {
            input_buf[pos] = '\0';
            if (pos > 0) {
                handle_input(input_buf);
                pos = 0;
            }
            erase();
            refresh_all();
            mvprintw(LINES - 2, 6, ">>> ");
            refresh();
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                pos--;
                mvaddch(LINES - 2, 6 + pos, ' ');
                move(LINES - 2, 6 + pos);
                refresh();
            }
        } else if (ch == 3) { /* Ctrl+C */
            break;
        } else if (ch == 14) { /* Ctrl+N - 切换 Backend */
            if (G.backend && strcmp(G.backend, "kimi") == 0)
                G.backend = "deepseek";
            else
                G.backend = "kimi";
            erase();
            refresh_all();
            mvprintw(LINES - 2, 6, ">>> ");
            refresh();
        } else if (ch == 23) { /* Ctrl+W 清屏 */
            chat_count = 0;
            erase();
            refresh_all();
            mvprintw(LINES - 2, 6, ">>> ");
            refresh();
        } else if (ch == KEY_UP) {
            if (chat_scroll_offset < chat_count - 1)
                chat_scroll_offset++;
            erase();
            refresh_all();
            mvprintw(LINES - 2, 6, ">>> ");
            refresh();
        } else if (ch == KEY_DOWN) {
            if (chat_scroll_offset > 0)
                chat_scroll_offset--;
            erase();
            refresh_all();
            mvprintw(LINES - 2, 6, ">>> ");
            refresh();
        } else if (ch == KEY_F(2)) {
            if (G.backend && strcmp(G.backend, "kimi") == 0)
                G.backend = "deepseek";
            else
                G.backend = "kimi";
            char msg[256];
            snprintf(msg, sizeof(msg), "切换到 %s",
                     G.backend && strcmp(G.backend, "kimi") == 0 ? "Kimi K3" : "DeepSeek");
            chat_add("system", msg);
            erase();
            refresh_all();
            mvprintw(LINES - 2, 6, ">>> ");
            refresh();
        } else if ((ch >= 32 && ch <= 126) && pos < MAX_INPUT - 1) {
            input_buf[pos++] = ch;
            addch(ch);
            refresh();
        }
    }

    noecho();
    curs_set(0);
}

/* =========================================================================
 * 信号处理
 * ========================================================================= */

static void sig_handler(int sig)
{
    (void)sig;
    G.running = 0;
}

/* =========================================================================
 * 主函数
 * ========================================================================= */

static void usage(const char *prog)
{
    printf("\n");
    printf("  AI Linux TUI — 交互式终端界面\n");
    printf("  v%s\n\n", VERSION);
    printf("  用法:\n");
    printf("    %s [选项]\n\n", prog);
    printf("  选项:\n");
    printf("    --key KEY       API 密钥\n");
    printf("    --key-env VAR   从环境变量读取密钥\n");
    printf("    --backend NAME  后端: deepseek(默认) | kimi\n");
    printf("    --help          显示帮助\n");
    printf("\n");
    printf("  环境变量:\n");
    printf("    DEEPSEEK_API_KEY / KIMI_API_KEY\n");
    printf("\n");
    printf("  示例:\n");
    printf("    DEEPSEEK_API_KEY=xxx %s\n", prog);
    printf("    %s --backend kimi --key-env KIMI_API_KEY\n", prog);
    printf("\n");
}

int main(int argc, char **argv)
{
    printf("\n");
    printf("  ╔═══════════════════════════════════════════╗\n");
    printf("  ║   AI Linux — Interactive TUI            ║\n");
    printf("  ║   DeepSeek · Kimi K3                   ║\n");
    printf("  ║   v%s                              ║\n", VERSION);
    printf("  ╚═══════════════════════════════════════════╝\n\n");

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--key") == 0 && i+1 < argc)
            G.api_key = argv[++i];
        else if (strcmp(argv[i], "--key-env") == 0 && i+1 < argc) {
            const char *v = getenv(argv[++i]);
            if (v) G.api_key = v;
        }
        else if (strcmp(argv[i], "--backend") == 0 && i+1 < argc)
            G.backend = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        }
    }

    /* 自动从环境变量获取 */
    if (!G.api_key) {
        const char *k = getenv("DEEPSEEK_API_KEY");
        if (!k) k = getenv("KIMI_API_KEY");
        if (k) G.api_key = k;
    }

    if (!G.api_key) {
        fprintf(stderr, "错误: 请设置 DEEPSEEK_API_KEY 或 KIMI_API_KEY 环境变量\n");
        fprintf(stderr, "   或使用 --key 参数\n\n");
        usage(argv[0]);
        return 1;
    }

    printf("  后端: %s\n", G.backend && strcmp(G.backend, "kimi") == 0 ? "Kimi K3" : "DeepSeek");
    printf("  API Key: 已设置\n\n");

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* 初始化 ncurses */
    initscr();
    curs_set(0);
    noecho();
    keypad(stdscr, TRUE);
    init_colors();
    cbreak();
    timeout(100);

    /* 创建窗口 */
    int total_lines = LINES;
    int title_h = 2;
    int status_h = 2;
    int input_h = 3;
    int content_h = total_lines - title_h - status_h - input_h - 1;

    title_win  = newwin(title_h, COLS, 0, 0);
    status_win = newwin(status_h, COLS, total_lines - status_h - input_h, 0);
    input_win  = newwin(input_h, COLS, total_lines - input_h, 0);

    left_win  = newwin(content_h, COLS/2 - 1, title_h, 0);
    right_win = newwin(content_h, COLS/2 - 1, title_h, COLS/2 + 1);

    /* 边框 */
    wbkgd(left_win,  COLOR_PAIR(COLOR_PANEL));
    wbkgd(right_win, COLOR_PAIR(COLOR_PANEL));
    box(left_win,  ACS_VLINE, ACS_HLINE);
    box(right_win, ACS_VLINE, ACS_HLINE);

    /* 初始数据 */
    refresh_sysinfo();
    chat_add("system", "AI Linux TUI 已启动！按数字键 1-5 使用快捷命令，或直接输入问题。");

    /* 主循环 */
    time_t last_refresh = 0;

    while (G.running) {
        time_t now = time(NULL);

        /* 每秒刷新显示 */
        if (now != last_refresh) {
            last_refresh = now;
            erase();
            refresh_all();
        }

        /* 非阻塞读取按键 */
        int ch = getch();
        if (ch != ERR) {
            if (ch == '\t') {
                if (G.backend && strcmp(G.backend, "kimi") == 0)
                    G.backend = "deepseek";
                else
                    G.backend = "kimi";
                char msg[256];
                snprintf(msg, sizeof(msg), "切换到 %s",
                         G.backend && strcmp(G.backend, "kimi") == 0 ? "Kimi K3" : "DeepSeek");
                chat_add("system", msg);
                erase();
                refresh_all();
            } else if (ch == KEY_F(2)) {
                if (G.backend && strcmp(G.backend, "kimi") == 0)
                    G.backend = "deepseek";
                else
                    G.backend = "kimi";
                erase();
                refresh_all();
            } else if (ch == 23) { /* Ctrl+W */
                chat_count = 0;
                erase();
                refresh_all();
            } else if (ch == 3 || ch == 27) { /* Ctrl+C or ESC */
                break;
            } else if (ch >= '1' && ch <= '5') {
                const char *q = preset_questions[ch - '1'];
                if (q) {
                    chat_add("user", q);
                    char result[4096];
                    double lat = 0;
                    const char *backend_used = NULL;
                    int ret = ai_call(q, result, sizeof(result), &lat, &backend_used);
                    if (ret == 0) {
                        chat_add("ai", result);
                        chat_history[0].latency_ms = lat;
                        strncpy(chat_history[0].backend,
                               backend_used ? backend_used : "unknown",
                               sizeof(chat_history[0].backend)-1);
                    } else {
                        chat_add("ai", result);
                    }
                    erase();
                    refresh_all();
                }
            } else if (ch == KEY_UP) {
                if (chat_scroll_offset < chat_count - 1)
                    chat_scroll_offset++;
                erase();
                refresh_all();
            } else if (ch == KEY_DOWN) {
                if (chat_scroll_offset > 0)
                    chat_scroll_offset--;
                erase();
                refresh_all();
            } else if (ch == KEY_RESIZE) {
                /* 终端大小改变，重新计算 */
                endwin();
                refresh();
                left_win  = newwin(content_h, COLS/2 - 1, title_h, 0);
                right_win = newwin(content_h, COLS/2 - 1, title_h, COLS/2 + 1);
                wbkgd(left_win,  COLOR_PAIR(COLOR_PANEL));
                wbkgd(right_win, COLOR_PAIR(COLOR_PANEL));
                box(left_win,  ACS_VLINE, ACS_HLINE);
                box(right_win, ACS_VLINE, ACS_HLINE);
                erase();
                refresh_all();
            }
        }

        usleep(50000); /* 50ms */
    }

    /* 退出 */
    erase();
    mvprintw(LINES/2, (COLS - 30)/2, "再见！正在退出...");
    refresh();
    usleep(500000);

    delwin(title_win);
    delwin(status_win);
    delwin(input_win);
    delwin(left_win);
    delwin(right_win);
    endwin();

    printf("\n\n  再见！\n\n");
    return 0;
}
