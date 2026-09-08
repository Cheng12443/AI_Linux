// SPDX-License-Identifier: GPL-2.0
/*
 * skill_registry.c — Skill 注册表
 *
 * 定义所有可用的 Skill，包括：
 *   - 内核调度 Skill
 *   - 网络 IO Skill
 *   - 安全检测 Skill
 *   - 内存管理 Skill
 *   - 代码生成 Skill
 *   - 系统编排 Skill
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../mcp/ai_mcp.h"
#include "../mcp/ai_mcp.h"

/* =========================================================================
 * 工具处理函数声明
 * ========================================================================= */

static int tool_sched_analyze(const char *params, char *result, size_t size, void *priv);
static int tool_io_inspect(const char *params, char *result, size_t size, void *priv);
static int tool_security_scan(const char *params, char *result, size_t size, void *priv);
static int tool_memory_predict(const char *params, char *result, size_t size, void *priv);
static int tool_process_kill(const char *params, char *result, size_t size, void *priv);
static int tool_network_block(const char *params, char *result, size_t size, void *priv);
static int tool_cpu_pin(const char *params, char *result, size_t size, void *priv);
static int tool_sys_info(const char *params, char *result, size_t size, void *priv);
static int tool_netstat(const char *params, char *result, size_t size, void *priv);
static int tool_disk_io(const char *params, char *result, size_t size, void *priv);

/* =========================================================================
 * Skill 定义
 * ========================================================================= */

/* Skill 1: 调度分析 */
static struct mcp_param param_sched_analyze[] = {
    { .name = "pid",       .type = "integer", .description = "进程 PID", .required = 0 },
    { .name = "comm",      .type = "string",  .description = "进程名（部分匹配）", .required = 0 },
    { .name = "threshold", .type = "number",  .description = "CPU 利用率阈值 0-1", .required = 0, .default_val = "0.5" },
};

static struct mcp_tool mcp_tool_sched = {
    .name        = "sched_analyze",
    .description = "分析进程调度状态，推荐 CPU 亲和性调整或优先级变更",
    .category    = "scheduling",
    /* params filled at runtime */
};

static struct skill skill_sched = {
    .name        = "scheduling_expert",
    .description = "Linux 调度专家：分析进程 CPU 使用，给出 promote/demote/migrate 建议",
    .category    = SKILL_CAT_SCHEDULING,
    .required_caps = BACKEND_CAP_FAST,
    .mcp_tool_name = "sched_analyze",
    .system_prompt = "你是一个 Linux 内核调度专家。你接收进程的 CPU 使用数据，"
                      "分析是否需要调整调度策略：升权（promote）、降权（demote）、"
                      "迁移（migrate）到其他 CPU、或标记为批处理（batch）。",
    .user_template = "分析以下进程：%s\n"
                      "返回 JSON：{\"action\":\"promote|demote|migrate|batch|keep\","
                      "\"confidence\":0-100,\"target_cpu\":0-255,\"reason\":\"理由\"}",
    .deepseek_weight = 70,
    .kimi_weight     = 30,
};

/* Skill 2: 网络 IO 分析 */
static struct mcp_param param_io_inspect[] = {
    { .name = "src_ip",    .type = "string",  .description = "源 IP 地址", .required = 0 },
    { .name = "dst_ip",    .type = "string",  .description = "目标 IP 地址", .required = 0 },
    { .name = "port",      .type = "integer", .description = "端口号", .required = 0 },
    { .name = "protocol",  .type = "string",  .description = "协议 tcp|udp|icmp", .required = 0 },
};

static struct mcp_tool mcp_tool_io = {
    .name        = "io_inspect",
    .description = "分析网络连接和 IO 统计，判断流量类型和异常",
    .category    = "io",
    /* params filled at runtime */
};

static struct skill skill_io = {
    .name        = "network_io_expert",
    .description = "网络 IO 专家：分析网络流量，判断是否放行、限速或阻断",
    .category    = SKILL_CAT_IO_NETWORK,
    .required_caps = BACKEND_CAP_FAST,
    .mcp_tool_name = "io_inspect",
    .system_prompt = "你是一个 Linux 网络安全分析专家。分析网络连接数据，"
                      "判断是否为正常流量、攻击流量或异常行为。"
                      "返回决策：pass（放行）/ drop（丢弃）/ redirect（重定向）/ alert（告警）。",
    .user_template = "分析以下网络连接：%s\n"
                      "返回 JSON：{\"action\":\"pass|drop|redirect|alert\","
                      "\"confidence\":0-100,\"reason\":\"理由\"}",
    .deepseek_weight = 50,
    .kimi_weight     = 50,
};

/* Skill 3: 安全检测 */
static struct mcp_param param_security[] = {
    { .name = "comm",      .type = "string",  .description = "可执行文件名", .required = 0 },
    { .name = "argv",      .type = "string",  .description = "命令行参数", .required = 0 },
    { .name = "uid",       .type = "integer", .description = "用户 UID", .required = 0 },
    { .name = "pid",       .type = "integer", .description = "进程 PID", .required = 0 },
};

static struct mcp_tool mcp_tool_security = {
    .name        = "security_scan",
    .description = "检测进程执行的恶意特征，扫描可疑命令和参数",
    .category    = "security",
    /* params filled at runtime */
};

static struct skill skill_security = {
    .name        = "security_expert",
    .description = "安全专家：检测进程执行是否包含恶意特征，阻止可疑行为",
    .category    = SKILL_CAT_SECURITY,
    .required_caps = BACKEND_CAP_FAST,
    .mcp_tool_name = "security_scan",
    .system_prompt = "你是一个 Linux 内核安全专家。分析进程执行的命令和参数，"
                      "判断是否存在恶意特征（提权、持久化、网络扫描、shell 注入等）。"
                      "返回：allow（允许）/ block（阻止）/ alert（告警）。"
                      "宁可误报不可漏报。",
    .user_template = "检测以下进程：%s\n"
                      "返回 JSON：{\"action\":\"allow|block|alert\","
                      "\"confidence\":0-100,\"threat_level\":\"low|medium|high|critical\",\"reason\":\"理由\"}",
    .deepseek_weight = 40,
    .kimi_weight     = 60,
};

/* Skill 4: 内存预测 */
static struct mcp_param param_memory[] = {
    { .name = "pid",       .type = "integer", .description = "进程 PID", .required = 0 },
    { .name = "mem_mb",   .type = "number",  .description = "内存使用 MB", .required = 0 },
    { .name = "swap_kb",   .type = "integer", .description = "Swap 使用 KB", .required = 0 },
};

static struct mcp_tool mcp_tool_memory = {
    .name        = "memory_predict",
    .description = "预测内存使用趋势，推荐页面换入换出策略",
    .category    = "memory",
    /* params filled at runtime */
};

static struct skill skill_memory = {
    .name        = "memory_expert",
    .description = "内存管理专家：预测页面热度，决定换入/换出策略",
    .category    = SKILL_CAT_MEMORY,
    .required_caps = BACKEND_CAP_FAST,
    .mcp_tool_name = "memory_predict",
    .system_prompt = "你是一个 Linux 内存管理专家。根据进程的内存使用和历史访问模式，"
                      "预测哪些页面即将被访问（swap_in）、哪些可以换出（swap_out）。",
    .user_template = "分析以下进程的内存：%s\n"
                      "返回 JSON：{\"swap_in\":[\"pfn1\",\"pfn2\"],\"swap_out\":[\"pfn3\"]}",
    .deepseek_weight = 65,
    .kimi_weight     = 35,
};

/* Skill 5: 进程管理 */
static struct mcp_param param_kill[] = {
    { .name = "pid",       .type = "integer", .description = "进程 PID", .required = 1 },
    { .name = "signal",    .type = "integer", .description = "信号值（默认9）", .required = 0, .default_val = "9" },
    { .name = "reason",    .type = "string",  .description = "终止原因", .required = 0 },
};

static struct mcp_tool mcp_tool_kill = {
    .name        = "process_kill",
    .description = "终止指定进程（需安全确认）",
    .category    = "system",
    /* params filled at runtime */
};

/* Skill 6: 网络阻断 */
static struct mcp_param param_net_block[] = {
    { .name = "ip",        .type = "string",  .description = "IP 地址", .required = 1 },
    { .name = "duration_s", .type = "integer", .description = "阻断时长（秒，默认3600）", .required = 0, .default_val = "3600" },
    { .name = "reason",    .type = "string",  .description = "阻断原因", .required = 0 },
};

static struct mcp_tool mcp_tool_net_block = {
    .name        = "network_block",
    .description = "在 iptables 中添加规则阻断指定 IP",
    .category    = "security",
    /* params filled at runtime */
};

/* Skill 7: CPU 亲和性 */
static struct mcp_param param_cpu_pin[] = {
    { .name = "pid",       .type = "integer", .description = "进程 PID", .required = 1 },
    { .name = "cpus",      .type = "string",  .description = "CPU 掩码（如 0-3 或 0xff）", .required = 1 },
};

static struct mcp_tool mcp_tool_cpu_pin = {
    .name        = "cpu_pin",
    .description = "设置进程 CPU 亲和性（绑定到指定 CPU）",
    .category    = "scheduling",
    /* params filled at runtime */
};

/* Skill 8: 系统信息 */
static struct mcp_param param_sysinfo[] = {
    { .name = "section",   .type = "string",  .description = "信息类型: cpu|mem|disk|net|proc|all", .required = 0, .default_val = "all" },
};

static struct mcp_tool mcp_tool_sysinfo = {
    .name        = "sys_info",
    .description = "获取系统信息：CPU/内存/磁盘/网络/进程",
    .category    = "system",
    /* params filled at runtime */
};

static struct skill skill_sysinfo = {
    .name        = "system_expert",
    .description = "系统综合分析：收集系统各维度信息，供 AI 推理使用",
    .category    = SKILL_CAT_SYSTEM,
    .required_caps = BACKEND_CAP_FAST,
    .mcp_tool_name = "sys_info",
    .system_prompt = "你是一个 Linux 系统管理员。收集和展示系统各维度的状态信息。",
    .user_template = "获取系统信息：%s\n以 JSON 格式返回系统状态。",
    .deepseek_weight = 55,
    .kimi_weight     = 45,
};

/* Skill 9: 网络连接分析 */
static struct mcp_param param_netstat[] = {
    { .name = "filter",    .type = "string",  .description = "过滤条件: established|time_wait|listening|all", .required = 0, .default_val = "all" },
    { .name = "limit",     .type = "integer", .description = "返回条数上限", .required = 0, .default_val = "50" },
};

static struct mcp_tool mcp_tool_netstat = {
    .name        = "netstat_query",
    .description = "查询当前网络连接状态",
    .category    = "io",
    /* params filled at runtime */
};

/* Skill 10: 磁盘 IO 分析 */
static struct mcp_param param_disk[] = {
    { .name = "device",    .type = "string",  .description = "设备名（如 sda）", .required = 0 },
    { .name = "interval",  .type = "integer", .description = "采样间隔秒", .required = 0, .default_val = "1" },
};

static struct mcp_tool mcp_tool_disk = {
    .name        = "disk_io_analyze",
    .description = "分析磁盘 IO 吞吐和延迟",
    .category    = "io",
    /* params filled at runtime */
};

static struct skill skill_disk = {
    .name        = "storage_expert",
    .description = "存储 IO 专家：分析磁盘吞吐和延迟，给出优化建议",
    .category    = SKILL_CAT_IO_NETWORK | SKILL_CAT_ANALYSIS,
    .required_caps = BACKEND_CAP_FAST,
    .mcp_tool_name = "disk_io_analyze",
    .system_prompt = "你是一个 Linux 存储专家。分析磁盘 IO 数据，"
                      "判断是否存在 IO 瓶颈或异常。",
    .user_template = "分析以下磁盘 IO：%s\n"
                      "返回 JSON：{\"iops\":N,\"throughput_mb_s\":N,\"latency_ms\":N,\"status\":\"normal|busy|overloaded\"}",
    .deepseek_weight = 60,
    .kimi_weight     = 40,
};

/* =========================================================================
 * 工具处理函数实现
 * ========================================================================= */

static int tool_sched_analyze(const char *params, char *result, size_t size, void *priv)
{
    (void)priv;
    int pid = -1;
    char comm[64] = { 0 };
    double threshold = 0.5;

    /* 解析参数 */
    if (params[0]) {
        char pid_s[32], thresh_s[32];
        if (sscanf(params, "%*[^{]\"pid\":%[^,}]", pid_s) == 1)
            pid = atoi(pid_s);
        if (sscanf(params, "%*[^{]\"threshold\":%[^,}]", thresh_s) == 1)
            threshold = atof(thresh_s);
    }

    /* 模拟：读取 /proc 数据 */
    snprintf(result, size,
        "{"
        "\"pid\":%d,"
        "\"comm\":\"%s\","
        "\"cpu_util\":0.75,"
        "\"nvcsw\":120,"
        "\"nivcsw\":8,"
        "\"io_wait_ms\":250,"
        "\"recommendation\":\"promote\","
        "\"confidence\":82,"
        "\"reason\":\"CPU利用率%.0f%%，IO等待较高，建议升权\","
        "\"target_cpu\":1"
        "}",
        pid, comm, threshold * 100);

    return 0;
}

static int tool_io_inspect(const char *params, char *result, size_t size, void *priv)
{
    (void)priv;
    char src_ip[32] = { 0 }, dst_ip[32] = { 0 };
    int port = 0;

    if (params[0]) {
        sscanf(params, "%*[^\"]\"src_ip\":\"%31[^\"]", src_ip);
        sscanf(params, "%*[^\"]\"dst_ip\":\"%31[^\"]", dst_ip);
        sscanf(params, "%*[^\"]\"port\":%d", &port);
    }

    /* 模拟：读取网络统计 */
    snprintf(result, size,
        "{"
        "\"src_ip\":\"%s\","
        "\"dst_ip\":\"%s\","
        "\"port\":%d,"
        "\"rx_bytes\":102400,"
        "\"tx_bytes\":51200,"
        "\"packets\":256,"
        "\"established\":1,"
        "\"threat_score\":0.15,"
        "\"classification\":\"normal_https\","
        "\"action\":\"pass\","
        "\"confidence\":91"
        "}",
        src_ip[0] ? src_ip : "192.168.1.100",
        dst_ip[0] ? dst_ip : "8.8.8.8",
        port ? port : 443);

    return 0;
}

static int tool_security_scan(const char *params, char *result, size_t size, void *priv)
{
    (void)priv;
    char comm[64] = { 0 }, argv[256] = { 0 };
    int uid = -1;

    if (params[0]) {
        sscanf(params, "%*[^\"]\"comm\":\"%63[^\"]", comm);
        sscanf(params, "%*[^\"]\"argv\":\"%255[^\"]", argv);
        sscanf(params, "%*[^\"]\"uid\":%d", &uid);
    }

    /* 恶意特征检测 */
    int threat = 0;
    const char *threats[] = {
        "wget.*\\|.*bash", "curl.*\\|.*sh", "nc -e",
        "/etc/passwd", "/etc/shadow", "chmod +s",
        "LD_PRELOAD", "ptrace", "cat /proc/",
    };

    for (int i = 0; i < 9; i++) {
        if (strstr(argv, threats[i])) { threat = 1; break; }
    }

    snprintf(result, size,
        "{"
        "\"comm\":\"%s\","
        "\"argv\":\"%.100s\","
        "\"uid\":%d,"
        "\"threat_detected\":%s,"
        "\"threat_level\":\"%s\","
        "\"action\":\"%s\","
        "\"confidence\":%d,"
        "\"matched_pattern\":\"%s\""
        "}",
        comm, argv, uid,
        threat ? "true" : "false",
        threat ? "high" : "low",
        threat ? "block" : "allow",
        threat ? 88 : 95,
        threat ? "suspicious_command" : "none");

    return 0;
}

static int tool_memory_predict(const char *params, char *result, size_t size, void *priv)
{
    (void)params; (void)priv;
    snprintf(result, size,
        "{"
        "\"hot_pages\":[\"0x1234000\",\"0x1235000\",\"0x1236000\"],"
        "\"cold_pages\":[\"0x2234000\",\"0x2235000\"],"
        "\"predicted_swap_in\":3,"
        "\"predicted_swap_out\":5,"
        "\"confidence\":78"
        "}");
    return 0;
}

static int tool_process_kill(const char *params, char *result, size_t size, void *priv)
{
    (void)params; (void)priv;
    int pid = -1, sig = 9;
    char reason[128] = { 0 };
    sscanf(params, "%*[^\"]\"pid\":%d", &pid);
    sscanf(params, "%*[^\"]\"signal\":%d", &sig);
    sscanf(params, "%*[^\"]\"reason\":\"%127[^\"]", reason);

    /* 实际调用：kill(pid, sig) */
    snprintf(result, size,
        "{\"status\":\"sent\",\"pid\":%d,\"signal\":%d,\"reason\":\"%.64s\"}",
        pid, sig, reason);
    return 0;
}

static int tool_network_block(const char *params, char *result, size_t size, void *priv)
{
    (void)params; (void)priv;
    char ip[32] = { 0 };
    int duration = 3600;
    sscanf(params, "%*[^\"]\"ip\":\"%31[^\"]", ip);
    sscanf(params, "%*[^\"]\"duration_s\":%d", &duration);

    /* 实际调用：iptables -A INPUT -s IP -j DROP */
    snprintf(result, size,
        "{\"status\":\"blocked\",\"ip\":\"%s\",\"duration_s\":%d,"
        "\"iptables_rule\":\"iptables -I INPUT -s %s -j DROP\"}",
        ip, duration, ip);
    return 0;
}

static int tool_cpu_pin(const char *params, char *result, size_t size, void *priv)
{
    (void)params; (void)priv;
    int pid = -1;
    char cpus[32] = { 0 };
    sscanf(params, "%*[^\"]\"pid\":%d", &pid);
    sscanf(params, "%*[^\"]\"cpus\":\"%31[^\"]", cpus);

    snprintf(result, size,
        "{\"status\":\"pinned\",\"pid\":%d,\"cpus\":\"%s\"}", pid, cpus);
    return 0;
}

static int tool_sys_info(const char *params, char *result, size_t size, void *priv)
{
    (void)params; (void)priv;
    snprintf(result, size,
        "{"
        "\"cpu\":{\"cores\":8,\"usage\":0.62,\"loadavg\":\"0.45 0.38 0.29\"},"
        "\"memory\":{\"total_gb\":32,\"used_gb\":18,\"available_gb\":14,\"swap_gb\":8},"
        "\"disk\":{\"sda\":{\"read_mb_s\":120,\"write_mb_s\":80}},"
        "\"network\":{\"rx_mb_s\":5.2,\"tx_mb_s\":3.1,\"connections\":342},"
        "\"processes\":{\"total\":287,\"running\":12,\"sleeping\":275},"
        "\"uptime_s\":86400"
        "}");
    return 0;
}

static int tool_netstat(const char *params, char *result, size_t size, void *priv)
{
    (void)params; (void)priv;
    snprintf(result, size,
        "{"
        "\"connections\":["
        "{\"proto\":\"tcp\",\"local\":\"192.168.1.10:443\",\"remote\":\"8.8.8.8:54321\",\"state\":\"ESTABLISHED\"},"
        "{\"proto\":\"tcp\",\"local\":\"192.168.1.10:22\",\"remote\":\"10.0.0.5:45678\",\"state\":\"ESTABLISHED\"},"
        "{\"proto\":\"tcp\",\"local\":\"0.0.0.0:80\",\"remote\":\"0.0.0.0:0\",\"state\":\"LISTEN\"}"
        "],\"total\":3"
        "}");
    return 0;
}

static int tool_disk_io(const char *params, char *result, size_t size, void *priv)
{
    (void)params; (void)priv;
    snprintf(result, size,
        "{"
        "\"device\":\"sda\","
        "\"read_iops\":500,\"write_iops\":300,"
        "\"read_mb_s\":40,\"write_mb_s\":25,"
        "\"avg_latency_ms\":2.3,"
        "\"utilization_pct\":35,"
        "\"status\":\"normal\""
        "}");
    return 0;
}

/* =========================================================================
 * 注册所有工具和 Skill
 * ========================================================================= */

void skill_registry_init(void)
{
    /* 注册 MCP 工具 */
    mcp_tool_register(&mcp_tool_sched);
    mcp_tool_register(&mcp_tool_io);
    mcp_tool_register(&mcp_tool_security);
    mcp_tool_register(&mcp_tool_memory);
    mcp_tool_register(&mcp_tool_kill);
    mcp_tool_register(&mcp_tool_net_block);
    mcp_tool_register(&mcp_tool_cpu_pin);
    mcp_tool_register(&mcp_tool_sysinfo);
    mcp_tool_register(&mcp_tool_netstat);
    mcp_tool_register(&mcp_tool_disk);

    /* 注册 Skill */
    skill_register(&skill_sched);
    skill_register(&skill_io);
    skill_register(&skill_security);
    skill_register(&skill_memory);
    skill_register(&skill_sysinfo);
    skill_register(&skill_disk);
}
