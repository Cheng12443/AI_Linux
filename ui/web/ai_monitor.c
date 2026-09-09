// SPDX-License-Identifier: GPL-2.0
/*
 * ai_monitor.c — AI Linux 性能监控面板
 *
 * 功能：
 *   - 实时系统指标（CPU/内存/磁盘/网络/进程）
 *   - AI 推理性能监控（延迟/吞吐量/成功率）
 *   - Layer2 网关状态
 *   - Skill 路由统计
 *   - 决策日志时间线
 *   - 系统健康评分
 *
 * 编译：
 *   gcc -O2 -o ai-monitor ai_monitor.c -lpthread -lm
 *
 * 运行：
 *   ./ai-monitor --port 9090 --key $DEEPSEEK_API_KEY
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>

#define VERSION "1.0.0"
#define PORT    9090
#define MAX_FD  4096

/* =========================================================================
 * 全局配置
 * ========================================================================= */

static struct {
    int         port;
    int         running;
    const char *api_key;
    int         backend;       /* 0=deepseek, 1=kimi */
    int         refresh_ms;
    char        bind[64];
} G = {
    .port       = PORT,
    .bind       = "127.0.0.1",
    .api_key    = NULL,
    .backend    = 0,
    .refresh_ms = 1000,
};

/* =========================================================================
 * 数据收集
 * ========================================================================= */

typedef struct {
    /* CPU */
    unsigned long long cpu_user, cpu_system, cpu_idle, cpu_iowait;
    int               cpu_usage;

    /* 内存 */
    unsigned long long mem_total, mem_free, mem_available;
    unsigned long long swap_total, swap_free;
    int               mem_usage;

    /* 负载 */
    double load1, load5, load15;
    int    proc_running, proc_total;

    /* 网络 */
    unsigned long long net_rx, net_tx;

    /* 磁盘 */
    unsigned long long disk_read, disk_write;

    /* AI 性能 */
    int    ai_inferences;
    int    ai_errors;
    double ai_avg_latency_ms;
    double ai_p99_latency_ms;
    double ai_throughput_rps;  /* 请求/秒 */

    /* Layer2 */
    int    layer2_connected;
    int    layer2_backend;   /* 0=deepseek, 1=kimi */
    int    layer2_mode;      /* 0=disabled, 1=sync, 2=async, 3=override */
    double layer2_latency_ms;

    /* Skill 统计 */
    struct {
        int sched, io, sec, mem, code, orch;
        int deepseek_calls, kimi_calls;
    } skills;

    /* 健康评分 */
    int    health_score;     /* 0-100 */

    /* 时间 */
    time_t timestamp;
} MonitorData;

static MonitorData data;
static MonitorData history[3600]; /* 最近1小时 */
static int history_idx = 0;
static int history_count = 0;

static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;

/* =========================================================================
 * 系统数据读取
 * ========================================================================= */

static void read_cpu(MonitorData *d)
{
    static unsigned long long prev_user = 0, prev_sys = 0, prev_idle = 0, prev_iowait = 0;
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            unsigned long long u = 0, s = 0, i = 0, w = 0;
            sscanf(line + 5, "%llu %llu %llu %llu %llu", &u, &s, &s, &i, &w);
            unsigned long long total = (u - prev_user) + (s - prev_sys) +
                                      (i - prev_idle) + (w - prev_iowait);
            if (total > 0)
                d->cpu_usage = (int)((total - (i - prev_idle)) * 100 / total);
            prev_user = u; prev_sys = s; prev_idle = i; prev_iowait = w;
            d->cpu_user = u; d->cpu_system = s;
            d->cpu_idle = i; d->cpu_iowait = w;
            break;
        }
    }
    fclose(fp);
}

static void read_memory(MonitorData *d)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;
    char line[256];
    unsigned long long v;
    d->mem_total = d->mem_free = d->mem_available = 0;
    d->swap_total = d->swap_free = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %llu kB", &v) == 1) d->mem_total = v;
        else if (sscanf(line, "MemFree: %llu kB", &v) == 1) d->mem_free = v;
        else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) d->mem_available = v;
        else if (sscanf(line, "SwapTotal: %llu kB", &v) == 1) d->swap_total = v;
        else if (sscanf(line, "SwapFree: %llu kB", &v) == 1) d->swap_free = v;
    }
    fclose(fp);
    if (d->mem_total > 0)
        d->mem_usage = (int)((d->mem_total - d->mem_available) * 100 / d->mem_total);
}

static void read_loadavg(MonitorData *d)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return;
    char line[128];
    if (fgets(line, sizeof(line), fp))
        sscanf(line, "%lf %lf %lf %d/%d",
               &d->load1, &d->load5, &d->load15,
               &d->proc_running, &d->proc_total);
    fclose(fp);
}

static void read_network(MonitorData *d)
{
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return;
    d->net_rx = d->net_tx = 0;
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
        d->net_rx += rx;
        d->net_tx += tx;
    }
    fclose(fp);
}

static void read_disk(MonitorData *d)
{
    FILE *fp = fopen("/proc/diskstats", "r");
    if (!fp) return;
    d->disk_read = d->disk_write = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long long r = 0, w = 0;
        int n = sscanf(line, "%*u %*u %*s %llu %*u %*u %*u %llu", &r, &w);
        if (n == 2) { d->disk_read += r; d->disk_write += w; }
    }
    fclose(fp);
}

static void read_uptime(MonitorData *d)
{
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) return;
    char line[64];
    double up;
    if (fgets(line, sizeof(line), fp))
        sscanf(line, "%lf", &up);
    d->timestamp = time(NULL);
    fclose(fp);
}

/* 模拟 AI 统计（真实场景从 /proc/ai/stats 读取）*/
static void read_ai_stats(MonitorData *d)
{
    /* 模拟数据 */
    d->ai_inferences = (int)(time(NULL) % 10000);
    d->ai_errors = (int)(time(NULL) % 50);
    d->ai_avg_latency_ms = 15.5 + (time(NULL) % 30);
    d->ai_p99_latency_ms = d->ai_avg_latency_ms * 2.5;
    d->ai_throughput_rps = 100 + (time(NULL) % 500);

    d->layer2_connected = 1;
    d->layer2_backend = G.backend;
    d->layer2_mode = 2; /* async */
    d->layer2_latency_ms = d->ai_avg_latency_ms * 1.5;
}

/* 健康评分计算 */
static int calc_health_score(MonitorData *d)
{
    int score = 100;

    /* CPU */
    if (d->cpu_usage > 90) score -= 20;
    else if (d->cpu_usage > 70) score -= 10;

    /* 内存 */
    if (d->mem_usage > 90) score -= 20;
    else if (d->mem_usage > 70) score -= 10;

    /* 负载 */
    if (d->load1 > d->proc_total) score -= 15;

    /* AI 错误率 */
    if (d->ai_inferences > 0) {
        int err_rate = d->ai_errors * 100 / d->ai_inferences;
        if (err_rate > 20) score -= 15;
        else if (err_rate > 10) score -= 8;
    }

    /* 延迟 */
    if (d->ai_avg_latency_ms > 100) score -= 10;
    else if (d->ai_avg_latency_ms > 50) score -= 5;

    return score < 0 ? 0 : (score > 100 ? 100 : score);
}

static void collect_data(void)
{
    pthread_mutex_lock(&data_lock);
    read_cpu(&data);
    read_memory(&data);
    read_loadavg(&data);
    read_network(&data);
    read_disk(&data);
    read_uptime(&data);
    read_ai_stats(&data);
    data.health_score = calc_health_score(&data);
    pthread_mutex_unlock(&data_lock);
}

/* =========================================================================
 * 历史数据管理
 * ========================================================================= */

static void push_history(void)
{
    pthread_mutex_lock(&data_lock);
    history[history_idx] = data;
    history_idx = (history_idx + 1) % 3600;
    if (history_count < 3600) history_count++;
    pthread_mutex_unlock(&data_lock);
}

/* =========================================================================
 * HTTP 服务器
 * ========================================================================= */

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (!inet_pton(AF_INET, G.bind, &addr.sin_addr))
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    listen(fd, 128);
    set_nonblock(fd);
    return fd;
}

static void send_http(int fd, int status, const char *type,
                     const char *body, int body_len)
{
    char hdr[512];
    int len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, type, body_len);
    send(fd, hdr, len, 0);
    if (body_len > 0) send(fd, body, body_len, 0);
}

/* =========================================================================
 * HTML 页面
 * ========================================================================= */

static const char *html_page =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>AI Linux Monitor</title>\n"
"<style>\n"
"* { margin:0; padding:0; box-sizing:border-box; }\n"
"body { background:#050510; color:#e0e0e0; font-family:'SF Mono',Monaco,monospace; }\n"
".header { background:linear-gradient(135deg,#0a0a20 0%,#1a0a30 100%);"
"  padding:20px 30px; border-bottom:1px solid #222;\n"
"  display:flex; justify-content:space-between; align-items:center; }\n"
".logo { color:#00d4ff; font-size:22px; font-weight:bold; }\n"
".logo span { color:#666; font-size:12px; margin-left:10px; font-weight:normal; }\n"
".live-badge { display:flex; align-items:center; gap:8px; color:#4ade80; font-size:13px; }\n"
".live-dot { width:8px; height:8px; border-radius:50%; background:#4ade80;\n"
"  animation:pulse 1.5s infinite; }\n"
"@keyframes pulse { 0%,100% { opacity:1; } 50% { opacity:0.3; } }\n"
".container { max-width:1600px; margin:0 auto; padding:20px; }\n"
".grid { display:grid; grid-template-columns:repeat(4,1fr); gap:16px; margin-bottom:16px; }\n"
".card { background:#0a0a1a; border:1px solid #1a1a2a; border-radius:12px;\n"
"  padding:20px; position:relative; overflow:hidden; }\n"
".card::before { content:''; position:absolute; top:0; left:0; right:0; height:2px;\n"
"  background:linear-gradient(90deg,#00d4ff,#a855f7); }\n"
".card-title { color:#666; font-size:11px; text-transform:uppercase; letter-spacing:1px;\n"
"  margin-bottom:12px; }\n"
".card-value { font-size:36px; font-weight:bold; color:#fff; line-height:1; }\n"
".card-unit { font-size:16px; color:#666; }\n"
".card-sub { font-size:12px; color:#555; margin-top:8px; }\n"
".gauge { width:100%; height:6px; background:#1a1a2a; border-radius:3px; margin-top:10px;\n"
"  overflow:hidden; }\n"
".gauge-bar { height:100%; border-radius:3px; transition:width 0.5s ease; }\n"
".gauge-cpu { background:linear-gradient(90deg,#00d4ff,#00ff88); }\n"
".gauge-mem { background:linear-gradient(90deg,#a855f7,#ec4899); }\n"
".gauge-warn { background:linear-gradient(90deg,#fbbf24,#f97316); }\n"
".gauge-crit { background:linear-gradient(90deg,#ef4444,#dc2626); }\n"
"\n"
".health { text-align:center; padding:30px 0; }\n"
".health-score { font-size:72px; font-weight:bold; color:#4ade80; }\n"
".health-score.warn { color:#fbbf24; }\n"
".health-score.crit { color:#ef4444; }\n"
".health-label { color:#666; font-size:14px; margin-top:8px; }\n"
"\n"
".section { background:#0a0a1a; border:1px solid #1a1a2a; border-radius:12px;\n"
"  padding:20px; margin-bottom:16px; }\n"
".section-title { color:#00d4ff; font-size:14px; font-weight:bold; margin-bottom:16px;\n"
"  padding-bottom:10px; border-bottom:1px solid #1a1a2a; }\n"
"\n"
".stats-grid { display:grid; grid-template-columns:repeat(6,1fr); gap:12px; }\n"
".stat-item { text-align:center; padding:12px; background:#050510; border-radius:8px; }\n"
".stat-value { font-size:24px; font-weight:bold; color:#fff; }\n"
".stat-label { font-size:10px; color:#666; margin-top:4px; }\n"
"\n"
".chart { width:100%; height:120px; margin-top:10px; }\n"
".chart-bar { display:inline-block; width:4px; margin-right:2px;\n"
"  vertical-align:bottom; border-radius:2px 2px 0 0; }\n"
"\n"
".skill-bar { display:flex; height:24px; border-radius:6px; overflow:hidden;\n"
"  margin-bottom:8px; }\n"
".skill-bar div { display:flex; align-items:center; justify-content:center;\n"
"  font-size:11px; font-weight:bold; color:#000; }\n"
".skill-bar .deep { background:#00d4ff; }\n"
".skill-bar .kimi { background:#a855f7; }\n"
"\n"
".table { width:100%; border-collapse:collapse; font-size:13px; }\n"
".table th { text-align:left; color:#666; font-size:10px; text-transform:uppercase;\n"
"  padding:8px 12px; border-bottom:1px solid #1a1a2a; }\n"
".table td { padding:10px 12px; border-bottom:1px solid #111; }\n"
".table tr:hover td { background:#0a0a15; }\n"
".badge { display:inline-block; padding:2px 10px; border-radius:12px; font-size:11px;\n"
"  font-weight:bold; }\n"
".badge-ok { background:#0a2a1a; color:#4ade80; }\n"
".badge-warn { background:#2a1a0a; color:#fbbf24; }\n"
".badge-err { background:#2a0a0a; color:#ef4444; }\n"
".badge-info { background:#0a1a2a; color:#60a5fa; }\n"
"\n"
".tabs { display:flex; gap:4px; margin-bottom:16px; }\n"
".tab { padding:8px 16px; background:#0a0a1a; border:1px solid #1a1a2a;\n"
"  border-radius:8px 8px 0 0; cursor:pointer; font-size:13px; color:#666;\n"
"  transition:all 0.2s; }\n"
".tab:hover { color:#00d4ff; }\n"
".tab.active { color:#00d4ff; border-bottom-color:#00d4ff; background:#111; }\n"
".tab-content { display:none; }\n"
".tab-content.active { display:block; }\n"
"\n"
".footer { text-align:center; padding:30px; color:#333; font-size:12px; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"\n"
"<div class=\"header\">\n"
"  <div class=\"logo\">AI Linux Monitor <span>v"VERSION"</span></div>\n"
"  <div class=\"live-badge\"><span class=\"live-dot\"></span>实时</div>\n"
"</div>\n"
"\n"
"<div class=\"container\">\n"
"\n"
"<!-- 健康评分 -->\n"
"<div class=\"section\" style=\"margin-bottom:20px;\">\n"
"  <div class=\"health\">\n"
"    <div class=\"health-score\" id=\"health-score\">--</div>\n"
"    <div class=\"health-label\">系统健康评分</div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- 核心指标 -->\n"
"<div class=\"grid\">\n"
"  <div class=\"card\">\n"
"    <div class=\"card-title\">CPU 使用率</div>\n"
"    <div class=\"card-value\" id=\"cpu-val\">--<span class=\"card-unit\">%</span></div>\n"
"    <div class=\"gauge\"><div class=\"gauge-bar gauge-cpu\" id=\"cpu-bar\" style=\"width:0%\"></div></div>\n"
"    <div class=\"card-sub\" id=\"cpu-sub\"></div>\n"
"  </div>\n"
"\n"
"  <div class=\"card\">\n"
"    <div class=\"card-title\">内存使用率</div>\n"
"    <div class=\"card-value\" id=\"mem-val\">--<span class=\"card-unit\">%</span></div>\n"
"    <div class=\"gauge\"><div class=\"gauge-bar gauge-mem\" id=\"mem-bar\" style=\"width:0%\"></div></div>\n"
"    <div class=\"card-sub\" id=\"mem-sub\"></div>\n"
"  </div>\n"
"\n"
"  <div class=\"card\">\n"
"    <div class=\"card-title\">负载均值</div>\n"
"    <div class=\"card-value\" id=\"load-val\">--</div>\n"
"    <div class=\"card-sub\" id=\"load-sub\"></div>\n"
"  </div>\n"
"\n"
"  <div class=\"card\">\n"
"    <div class=\"card-title\">运行时间</div>\n"
"    <div class=\"card-value\" id=\"uptime-val\">--</div>\n"
"    <div class=\"card-sub\">系统在线</div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- AI 性能 -->\n"
"<div class=\"section\">\n"
"  <div class=\"section-title\">AI 推理性能</div>\n"
"  <div class=\"stats-grid\">\n"
"    <div class=\"stat-item\">\n"
"      <div class=\"stat-value\" id=\"ai-inferences\">--</div>\n"
"      <div class=\"stat-label\">推理总数</div>\n"
"    </div>\n"
"    <div class=\"stat-item\">\n"
"      <div class=\"stat-value\" id=\"ai-errors\">--</div>\n"
"      <div class=\"stat-label\">错误数</div>\n"
"    </div>\n"
"    <div class=\"stat-item\">\n"
"      <div class=\"stat-value\" id=\"ai-avg-latency\">--</div>\n"
"      <div class=\"stat-label\">平均延迟 (ms)</div>\n"
"    </div>\n"
"    <div class=\"stat-item\">\n"
"      <div class=\"stat-value\" id=\"ai-p99\">--</div>\n"
"      <div class=\"stat-label\">P99 延迟 (ms)</div>\n"
"    </div>\n"
"    <div class=\"stat-item\">\n"
"      <div class=\"stat-value\" id=\"ai-rps\">--</div>\n"
"      <div class=\"stat-label\">吞吐量 (req/s)</div>\n"
"    </div>\n"
"    <div class=\"stat-item\">\n"
"      <div class=\"stat-value\" id=\"layer2-status\">--</div>\n"
"      <div class=\"stat-label\">Layer2 状态</div>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- Skill 路由 -->\n"
"<div class=\"section\">\n"
"  <div class=\"section-title\">Skill 路由分布</div>\n"
"  <div id=\"skill-bars\"></div>\n"
"</div>\n"
"\n"
"<!-- 决策日志 -->\n"
"<div class=\"section\">\n"
"  <div class=\"section-title\">决策日志</div>\n"
"  <table class=\"table\">\n"
"    <thead>\n"
"      <tr>\n"
"        <th>时间</th><th>域</th><th>后端</th><th>决策</th>\n"
"        <th>置信度</th><th>延迟</th><th>状态</th>\n"
"      </tr>\n"
"    </thead>\n"
"    <tbody id=\"log-body\"></tbody>\n"
"  </table>\n"
"</div>\n"
"\n"
"</div>\n"
"\n"
"<div class=\"footer\">\n"
"  AI Linux Monitor | DeepSeek + Kimi K3 | 实时刷新\n"
"</div>\n"
"\n"
"<script>\n"
"async function refresh() {\n"
"  try {\n"
"    const r = await fetch('/api/metrics');\n"
"    const d = await r.json();\n"
"\n"
"    // 健康评分\n"
"    const hs = document.getElementById('health-score');\n"
"    hs.textContent = d.health_score;\n"
"    hs.className = 'health-score ' + (d.health_score >= 80 ? '' : d.health_score >= 60 ? 'warn' : 'crit');\n"
"\n"
"    // CPU\n"
"    document.getElementById('cpu-val').innerHTML = d.cpu.usage + '<span class=\"card-unit\">%</span>';\n"
"    document.getElementById('cpu-bar').style.width = d.cpu.usage + '%';\n"
"    document.getElementById('cpu-sub').textContent = 'user:' + (d.cpu.user/1e7).toFixed(1) + ' sys:' + (d.cpu.system/1e7).toFixed(1);\n"
"\n"
"    // 内存\n"
"    document.getElementById('mem-val').innerHTML = d.memory.usage + '<span class=\"card-unit\">%</span>';\n"
"    document.getElementById('mem-bar').style.width = d.memory.usage + '%';\n"
"    document.getElementById('mem-sub').textContent =\n"
"      Math.round(d.memory.total/1024) + 'G total / ' + Math.round(d.memory.available/1024) + 'G avail';\n"
"\n"
"    // 负载\n"
"    document.getElementById('load-val').textContent = d.load1.toFixed(2);\n"
"    document.getElementById('load-sub').textContent = d.proc_running + '/' + d.proc_total + ' 进程';\n"
"\n"
"    // 运行时间\n"
"    let up = d.uptime_s || 0;\n"
"    let days = Math.floor(up/86400);\n"
"    let hrs = Math.floor((up%86400)/3600);\n"
"    let mins = Math.floor((up%3600)/60);\n"
"    document.getElementById('uptime-val').textContent = days + 'd ' + hrs + 'h ' + mins + 'm';\n"
"\n"
"    // AI 统计\n"
"    document.getElementById('ai-inferences').textContent = d.ai_inferences.toLocaleString();\n"
"    document.getElementById('ai-errors').textContent = d.ai_errors;\n"
"    document.getElementById('ai-avg-latency').textContent = d.ai_avg_latency_ms.toFixed(1);\n"
"    document.getElementById('ai-p99').textContent = d.ai_p99_latency_ms.toFixed(1);\n"
"    document.getElementById('ai-rps').textContent = d.ai_throughput_rps;\n"
"    document.getElementById('layer2-status').textContent = d.layer2_connected ? '● 在线' : '○ 离线';\n"
"\n"
"    // Skill 路由\n"
"    const skills = d.skills || {};\n"
"    const skillNames = ['sched','io','sec','mem','code','orch'];\n"
"    let skillHtml = '';\n"
"    for (let s of skillNames) {\n"
"      let deep = skills[s] || 50;\n"
"      let kimi = 100 - deep;\n"
"      skillHtml += '<div style=\"margin-bottom:12px\"><div style=\"font-size:12px;color:#666;margin-bottom:4px\">' + s.toUpperCase() + '</div>';\n"
"      skillHtml += '<div class=\"skill-bar\"><div class=\"deep\" style=\"flex:' + deep + '\">' + deep + '%</div>';\n"
"      skillHtml += '<div class=\"kimi\" style=\"flex:' + kimi + '\">' + kimi + '%</div></div></div>';\n"
"    }\n"
"    document.getElementById('skill-bars').innerHTML = skillHtml;\n"
"\n"
"    // 模拟日志\n"
"    let logs = [];\n"
"    for (let i = 0; i < 10; i++) {\n"
"      logs.push({\n"
"        time: new Date(Date.now() - i*3000).toLocaleTimeString(),\n"
"        domain: ['sched','io','sec','mem'][i%4],\n"
"        backend: i%2 ? 'Kimi' : 'DeepSeek',\n"
"        decision: ['promote','keep','migrate','demote','pass'][i%5],\n"
"        confidence: 70 + Math.floor(Math.random()*30),\n"
"        latency: (10 + Math.random()*50).toFixed(1),\n"
"        status: i%5 === 0 ? 'warn' : 'ok'\n"
"      });\n"
"    }\n"
"\n"
"    let logHtml = logs.map(l => `\n"
"      <tr>\n"
"        <td>${l.time}</td>\n"
"        <td>${l.domain}</td>\n"
"        <td>${l.backend}</td>\n"
"        <td>${l.decision}</td>\n"
"        <td>${l.confidence}%</td>\n"
"        <td>${l.latency}ms</td>\n"
"        <td><span class=\"badge badge-${l.status}\">${l.status}</span></td>\n"
"      </tr>\n"
"    `).join('');\n"
"    document.getElementById('log-body').innerHTML = logHtml;\n"
"  } catch(e) { console.error('refresh error', e); }\n"
"}\n"
"\n"
"refresh();\n"
"setInterval(refresh, 1000);\n"
"</script>\n"
"</body>\n"
"</html>";

/* =========================================================================
 * API 处理
 * ========================================================================= */

static void handle_api_metrics(int fd)
{
    char buf[8192];
    pthread_mutex_lock(&data_lock);

    snprintf(buf, sizeof(buf),
        "{"
        "\"health_score\":%d,"
        "\"cpu\":{\"usage\":%d,\"user\":%llu,\"system\":%llu},"
        "\"memory\":{\"usage\":%d,\"total\":%llu,\"available\":%llu},"
        "\"load1\":%.2f,\"load5\":%.2f,\"load15\":%.2f,"
        "\"proc_running\":%d,\"proc_total\":%d,"
        "\"uptime_s\":%.0f,"
        "\"ai_inferences\":%d,\"ai_errors\":%d,"
        "\"ai_avg_latency_ms\":%.1f,\"ai_p99_latency_ms\":%.1f,"
        "\"ai_throughput_rps\":%.1f,"
        "\"layer2_connected\":%d,\"layer2_mode\":%d,"
        "\"layer2_latency_ms\":%.1f,"
        "\"skills\":{\"sched\":%d,\"io\":%d,\"sec\":%d,"
        "\"mem\":%d,\"code\":%d,\"orch\":%d},"
        "\"timestamp\":%lld"
        "}",
        data.health_score,
        data.cpu_usage, data.cpu_user, data.cpu_system,
        data.mem_usage, data.mem_total, data.mem_available,
        data.load1, data.load5, data.load15,
        data.proc_running, data.proc_total,
        (double)(time(NULL)),
        data.ai_inferences, data.ai_errors,
        data.ai_avg_latency_ms, data.ai_p99_latency_ms,
        data.ai_throughput_rps,
        data.layer2_connected, data.layer2_mode,
        data.layer2_latency_ms,
        data.skills.sched, data.skills.io, data.skills.sec,
        data.skills.mem, data.skills.code, data.skills.orch,
        (long long)time(NULL));

    pthread_mutex_unlock(&data_lock);
    send_http(fd, 200, "application/json", buf, strlen(buf));
}

/* =========================================================================
 * 请求处理
 * ========================================================================= */

static void handle_request(int fd, const char *path)
{
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        send_http(fd, 200, "text/html; charset=utf-8",
                  html_page, strlen(html_page));
    } else if (strcmp(path, "/api/metrics") == 0) {
        handle_api_metrics(fd);
    } else if (strncmp(path, "/api/", 5) == 0) {
        send_http(fd, 404, "application/json", "{\"error\":\"not found\"}", 19);
    } else {
        send_http(fd, 404, "text/plain", "404 Not Found", 13);
    }
}

static void *handle_conn(void *arg)
{
    int fd = *(int *)arg;
    free(arg);

    char buf[8192];
    int buf_len = 0;

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 5000);
    if (ret <= 0) { close(fd); return NULL; }

    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(fd); return NULL; }
    buf[n] = '\0';

    /* 解析请求路径 */
    char method[16], path[512];
    if (sscanf(buf, "%15s %511s", method, path) == 2) {
        handle_request(fd, path);
    } else {
        send_http(fd, 400, "text/plain", "400 Bad Request", 15);
    }

    close(fd);
    return NULL;
}

/* =========================================================================
 * 主循环
 * ========================================================================= */

static void *data_collector(void *arg)
{
    (void)arg;
    while (G.running) {
        collect_data();
        push_history();
        usleep(G.refresh_ms * 1000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    printf("\n");
    printf("  ╔═══════════════════════════════════════════════╗\n");
    printf("  ║   AI Linux — Performance Monitor             ║\n");
    printf("  ║   v%s                                 ║\n", VERSION);
    printf("  ╚═══════════════════════════════════════════════╝\n\n");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i+1 < argc)
            G.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i+1 < argc)
            strncpy(G.bind, argv[++i], sizeof(G.bind) - 1);
        else if (strcmp(argv[i], "--key") == 0 && i+1 < argc)
            G.api_key = argv[++i];
        else if (strcmp(argv[i], "--backend") == 0 && i+1 < argc) {
            if (strcmp(argv[i+1], "kimi") == 0) G.backend = 1;
            i++;
        }
        else if (strcmp(argv[i], "--refresh") == 0 && i+1 < argc)
            G.refresh_ms = atoi(argv[++i]);
    }

    printf("  监听: http://%s:%d\n", G.bind, G.port);
    printf("  后端: %s\n", G.backend ? "Kimi K3" : "DeepSeek");
    printf("  刷新: %dms\n\n", G.refresh_ms);

    signal(SIGINT,  (void *)1);
    signal(SIGTERM, (void *)1);

    G.running = 1;

    /* 启动数据收集线程 */
    pthread_t collector;
    pthread_create(&collector, NULL, data_collector, NULL);

    /* 创建监听 */
    int listen_fd = create_listen(G.port);
    if (listen_fd < 0) {
        fprintf(stderr, "错误: 无法监听端口 %d\n", G.port);
        return 1;
    }

    while (G.running) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int client = accept(listen_fd, (struct sockaddr *)&addr, &len);
        if (client < 0) continue;

        int *pfd = malloc(sizeof(int));
        *pfd = client;
        pthread_t th;
        pthread_create(&th, NULL, handle_conn, pfd);
        pthread_detach(th);
    }

    close(listen_fd);
    pthread_join(collector, NULL);

    printf("\n  再见！\n\n");
    return 0;
}
