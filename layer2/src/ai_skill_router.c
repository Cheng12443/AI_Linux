// SPDX-License-Identifier: GPL-2.0
/*
 * ai_skill_router.c — Skill 路由引擎
 *
 * 核心职责：
 *   1. 解析用户请求，识别 Skill 类型
 *   2. 根据 Skill 类型选择最优 Backend（DeepSeek / Kimi）
 *   3. 组装提示词（含 MCP 工具调用上下文）
 *   4. 调用选定的 Backend API
 *   5. 解析响应，执行 MCP 工具调用
 *   6. 返回最终结果
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>
#include "ai_mcp.h"
#include "../skills/skill_registry.c"

/* =========================================================================
 * Skill 路由表
 * ========================================================================= */

typedef struct {
    const char *keyword;          /* 请求中的关键词 */
    unsigned int category;        /* SKILL_CAT_* */
    int priority;                  /* 优先级（数字越大优先级越高）*/
} SkillMatchRule;

/* 关键词 → Skill 类别映射 */
static const SkillMatchRule match_rules[] = {
    /* 调度 */
    { "调度",     SKILL_CAT_SCHEDULING,   90 },
    { "cpu",     SKILL_CAT_SCHEDULING,   60 },
    { "进程",     SKILL_CAT_SCHEDULING,   50 },
    { "优先级",   SKILL_CAT_SCHEDULING,   70 },
    { "时间片",   SKILL_CAT_SCHEDULING,   80 },
    { "迁移",     SKILL_CAT_SCHEDULING,   85 },

    /* 网络 IO */
    { "网络",     SKILL_CAT_IO_NETWORK,   80 },
    { "流量",     SKILL_CAT_IO_NETWORK,   75 },
    { "连接",     SKILL_CAT_IO_NETWORK,   70 },
    { "端口",     SKILL_CAT_IO_NETWORK,   65 },
    { "带宽",     SKILL_CAT_IO_NETWORK,   60 },
    { "IO",      SKILL_CAT_IO_NETWORK,   55 },
    { "磁盘",     SKILL_CAT_IO_NETWORK,   50 },
    { "读写",     SKILL_CAT_IO_NETWORK,   65 },

    /* 安全 */
    { "安全",     SKILL_CAT_SECURITY,    90 },
    { "攻击",     SKILL_CAT_SECURITY,    95 },
    { "入侵",     SKILL_CAT_SECURITY,    95 },
    { "恶意",     SKILL_CAT_SECURITY,    90 },
    { "阻断",     SKILL_CAT_SECURITY,    80 },
    { "阻断IP",  SKILL_CAT_SECURITY,    85 },
    { "扫描",     SKILL_CAT_SECURITY,    70 },
    { "漏洞",     SKILL_CAT_SECURITY,    85 },
    { "权限",     SKILL_CAT_SECURITY,    60 },

    /* 内存 */
    { "内存",     SKILL_CAT_MEMORY,      80 },
    { "swap",    SKILL_CAT_MEMORY,      85 },
    { "页面",     SKILL_CAT_MEMORY,      80 },
    { "OOM",     SKILL_CAT_MEMORY,      90 },

    /* 系统 */
    { "系统",     SKILL_CAT_SYSTEM,      50 },
    { "负载",     SKILL_CAT_SYSTEM,      70 },
    { "CPU使用", SKILL_CAT_SYSTEM,      65 },

    /* 代码 */
    { "代码",     SKILL_CAT_CODE,        80 },
    { "shell",   SKILL_CAT_CODE,        70 },
    { "脚本",     SKILL_CAT_CODE,        60 },
    { "生成",     SKILL_CAT_CODE,        50 },

    /* 编排（多步） */
    { "分析",     SKILL_CAT_ANALYSIS,    60 },
    { "报告",     SKILL_CAT_ANALYSIS,    50 },
    { "优化",     SKILL_CAT_ORCHESTRATE, 70 },
    { "方案",     SKILL_CAT_ORCHESTRATE, 60 },
};

/* =========================================================================
 * Skill → MCP 工具映射
 * ========================================================================= */

typedef struct {
    unsigned int category;
    const char *mcp_tool;
    const char *prompt_suffix;
} SkillToolMapping;

static const SkillToolMapping tool_mappings[] = {
    { SKILL_CAT_SCHEDULING,   "sched_analyze",    "使用 sched_analyze 工具分析进程调度。" },
    { SKILL_CAT_IO_NETWORK,   "io_inspect",       "使用 io_inspect 工具检查网络流量。" },
    { SKILL_CAT_IO_NETWORK,   "netstat_query",    "使用 netstat_query 查询连接。" },
    { SKILL_CAT_IO_NETWORK,   "disk_io_analyze",  "使用 disk_io_analyze 分析磁盘。" },
    { SKILL_CAT_SECURITY,     "security_scan",     "使用 security_scan 检测恶意行为。" },
    { SKILL_CAT_SECURITY,     "network_block",     "使用 network_block 阻断威胁。" },
    { SKILL_CAT_MEMORY,       "memory_predict",    "使用 memory_predict 预测页面。" },
    { SKILL_CAT_SYSTEM,       "sys_info",          "使用 sys_info 获取系统信息。" },
    { SKILL_CAT_CODE,         "sys_info",          "使用 sys_info 和 shell 生成代码。" },
};

/* =========================================================================
 * Backend 配置（来自环境变量）
 * ========================================================================= */

typedef struct {
    int  backend;          /* 0=DeepSeek, 1=Kimi */
    char model[64];
    char endpoint[256];
    char api_key[256];
    int  weight_sched;     /* 调度任务权重 */
    int  weight_io;
    int  weight_sec;
    int  weight_mem;
    int  weight_code;
    int  weight_orch;
} BackendConfig;

static BackendConfig deepseek_cfg = {
    .backend        = 0,
    .model          = "deepseek-chat",
    .endpoint       = "https://api.deepseek.com/v1/chat/completions",
    .api_key        = { 0 },
    .weight_sched   = 70,
    .weight_io      = 50,
    .weight_sec     = 40,
    .weight_mem     = 65,
    .weight_code    = 80,
    .weight_orch    = 30,
};

static BackendConfig kimi_cfg = {
    .backend        = 1,
    .model          = "moonshot-v1-8k",
    .endpoint       = "https://api.moonshot.cn/v1/chat/completions",
    .api_key        = { 0 },
    .weight_sched   = 30,
    .weight_io      = 50,
    .weight_sec     = 60,
    .weight_mem     = 35,
    .weight_code    = 20,
    .weight_orch    = 70,
};

/* =========================================================================
 * Skill 路由分析
 * ========================================================================= */

/*
 * skill_analyze — 分析用户请求，确定 Skill 类别
 *
 * 返回：SKILL_CAT_* 位掩码
 */
static unsigned int skill_analyze(const char *request)
{
    if (!request) return SKILL_CAT_SYSTEM;

    unsigned int result = 0;
    int best_priority = 0;

    for (size_t i = 0; i < sizeof(match_rules)/sizeof(match_rules[0]); i++) {
        const SkillMatchRule *rule = &match_rules[i];
        if (strstr(request, rule->keyword)) {
            if (rule->priority > best_priority) {
                result = rule->category;
                best_priority = rule->priority;
            } else if (rule->priority == best_priority) {
                result |= rule->category;
            }
        }
    }

    /* 默认为系统综合分析 */
    if (result == 0) result = SKILL_CAT_SYSTEM;

    return result;
}

/*
 * skill_route — 根据 Skill 类别选择 Backend
 */
static int skill_route(unsigned int category, struct routing_decision *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    /* 计算两个 backend 的得分 */
    int deepseek_score = 0;
    int kimi_score = 0;

    #define SCORE(cat, cfg_field) do { \
        if (category & (cat)) { \
            deepseek_score += deepseek_cfg.cfg_field; \
            kimi_score += kimi_cfg.cfg_field; \
        } \
    } while(0)

    SCORE(SKILL_CAT_SCHEDULING,   weight_sched);
    SCORE(SKILL_CAT_IO_NETWORK,   weight_io);
    SCORE(SKILL_CAT_SECURITY,     weight_sec);
    SCORE(SKILL_CAT_MEMORY,       weight_mem);
    SCORE(SKILL_CAT_CODE,         weight_code);
    SCORE(SKILL_CAT_ANALYSIS,     weight_io);
    SCORE(SKILL_CAT_ORCHESTRATE,  weight_orch);
    SCORE(SKILL_CAT_SYSTEM,       weight_sched);

    #undef SCORE

    /* 选择得分更高的 */
    if (deepseek_score >= kimi_score) {
        out->backend = 0;
        strcpy(out->model, deepseek_cfg.model);
        out->confidence = deepseek_score * 100 /
            MAX(deepseek_score + kimi_score, 1);
        snprintf(out->reason, sizeof(out->reason),
                 "DeepSeek %d vs Kimi %d", deepseek_score, kimi_score);
    } else {
        out->backend = 1;
        strcpy(out->model, kimi_cfg.model);
        out->confidence = kimi_score * 100 /
            MAX(deepseek_score + kimi_score, 1);
        snprintf(out->reason, sizeof(out->reason),
                 "Kimi %d vs DeepSeek %d", kimi_score, deepseek_score);
    }

    /* 覆盖默认模型 */
    if (category & SKILL_CAT_CODE)
        strcpy(out->model, "deepseek-coder");
    if (category & SKILL_CAT_ORCHESTRATE)
        strcpy(out->model, "moonshot-v1-32k");

    /* 设置能力 */
    out->caps = BACKEND_CAP_FAST;
    if (category & SKILL_CAT_CODE)
        out->caps |= BACKEND_CAP_CODING;
    if (category & SKILL_CAT_ORCHESTRATE)
        out->caps |= BACKEND_CAP_LONG_CTX;

    return 0;
}

/* =========================================================================
 * MCP 工具调用
 * ========================================================================= */

/*
 * skill_execute_tool — 执行 MCP 工具
 *
 * 从 skill 中提取工具名，构建调用参数，执行工具
 */
static int skill_execute_tool(const char *tool_name,
                             const char *request_context,
                             char *result, size_t result_size)
{
    /* 构造模拟参数（从请求中提取关键信息）*/
    char params[1024] = "{";
    int first = 1;

    /* 尝试提取 PID */
    const char *pid_p = strstr(request_context, "PID");
    if (pid_p) {
        int pid = atoi(pid_p + 3);
        if (pid > 0) {
            if (!first) strcat(params, ",");
            snprintf(params + strlen(params), sizeof(params) - strlen(params),
                    "\"pid\":%d", pid);
            first = 0;
        }
    }

    /* 尝试提取 IP */
    const char *ip_p = strstr(request_context, "IP");
    if (ip_p) {
        char ip[32];
        int n = sscanf(ip_p + 2, "%31s", ip);
        if (n == 1 && strchr(ip, '.')) {
            if (!first) strcat(params, ",");
            snprintf(params + strlen(params), sizeof(params) - strlen(params),
                    "\"ip\":\"%s\"", ip);
            first = 0;
        }
    }

    /* 尝试提取进程名 */
    const char *comm_p = strstr(request_context, "进程");
    if (comm_p) {
        char comm[32];
        int n = sscanf(comm_p, "%*[进程] %31s", comm);
        if (n == 1) {
            if (!first) strcat(params, ",");
            snprintf(params + strlen(params), sizeof(params) - strlen(params),
                    "\"comm\":\"%s\"", comm);
            first = 0;
        }
    }

    strcat(params, "}");

    /* 调用 MCP */
    struct mcp_call_result cr;
    int ret = mcp_call_tool(tool_name, params, &cr);

    if (ret == 0) {
        snprintf(result, result_size, "%s", cr.content);
    } else {
        snprintf(result, result_size,
                "{\"error\":\"tool '%s' failed: %s\"}", tool_name, cr.error);
    }

    return ret;
}

/* =========================================================================
 * 提示词构建
 * ========================================================================= */

/*
 * build_skill_prompt — 构建 Skill 增强提示词
 *
 * 1. 确定 Skill 类别
 * 2. 选择对应的 MCP 工具
 * 3. 追加工具调用说明
 * 4. 注入系统提示词
 */
static void build_skill_prompt(char *out, size_t out_size,
                               const char *user_request,
                               unsigned int category)
{
    const char *system_prompt = "";
    const char *tool_hints = "";

    /* 根据类别选择系统提示词 */
    switch (category) {
    case SKILL_CAT_SCHEDULING:
        system_prompt = "你是一个运行在 Linux 内核中的 AI 调度专家。"
                        "你可以通过工具分析进程 CPU 使用，调整调度策略。"
                        "可用的 MCP 工具：sched_analyze（分析调度）、cpu_pin（绑定 CPU）。";
        tool_hints = "\n\nMCP 工具使用说明：\n"
                     "1. sched_analyze(pid=xxx) — 分析指定进程的调度状态\n"
                     "2. cpu_pin(pid=xxx, cpus='0-3') — 绑定进程到指定 CPU\n"
                     "3. sys_info — 获取系统整体状态\n";
        break;

    case SKILL_CAT_IO_NETWORK:
        system_prompt = "你是一个 Linux 网络和 IO 专家。"
                        "分析网络流量和磁盘 IO，判断是否存在异常。"
                        "可用的 MCP 工具：io_inspect、netstat_query、disk_io_analyze。";
        tool_hints = "\n\nMCP 工具使用说明：\n"
                     "1. io_inspect(src_ip=x, dst_ip=y, port=n) — 分析连接\n"
                     "2. netstat_query(filter='established') — 查询连接状态\n"
                     "3. disk_io_analyze(device='sda') — 分析磁盘 IO\n";
        break;

    case SKILL_CAT_SECURITY:
        system_prompt = "你是一个 Linux 内核安全专家。"
                        "检测恶意行为，发现威胁立即阻断。"
                        "可用的 MCP 工具：security_scan、network_block。";
        tool_hints = "\n\nMCP 工具使用说明：\n"
                     "1. security_scan(comm='xxx', argv='xxx') — 检测恶意特征\n"
                     "2. network_block(ip='x.x.x.x', duration_s=3600) — 阻断 IP\n"
                     "3. process_kill(pid=xxx, signal=9) — 终止进程\n"
                     "注意：宁可误报不可漏报！";
        break;

    case SKILL_CAT_MEMORY:
        system_prompt = "你是一个 Linux 内存管理专家。"
                        "分析页面热度，预测 swap 行为。"
                        "可用的 MCP 工具：memory_predict、sys_info。";
        tool_hints = "\n\nMCP 工具使用说明：\n"
                     "1. memory_predict(pid=xxx) — 预测页面热度\n"
                     "2. sys_info — 获取内存状态\n";
        break;

    case SKILL_CAT_CODE:
        system_prompt = "你是一个 Linux 系统程序员。"
                        "根据需求生成 shell 脚本或内核模块代码。";
        tool_hints = "\n\nMCP 工具使用说明：\n"
                     "1. sys_info — 获取系统环境信息\n"
                     "2. netstat_query — 获取网络连接（生成网络相关代码时）\n";
        break;

    case SKILL_CAT_ORCHESTRATE:
        system_prompt = "你是一个 Linux 系统架构师。"
                        "分析多维度系统数据，制定综合优化方案。"
                        "你可以使用所有 MCP 工具。";
        tool_hints = "\n\nMCP 工具使用说明：\n"
                     "1. sys_info — 系统综合信息\n"
                     "2. sched_analyze — 调度分析\n"
                     "3. io_inspect — 网络 IO 分析\n"
                     "4. disk_io_analyze — 磁盘分析\n"
                     "5. memory_predict — 内存预测\n"
                     "6. security_scan — 安全检测\n";
        break;

    default:
        system_prompt = "你是一个 Linux 系统管理员。"
                        "回答系统管理问题，可使用 MCP 工具获取实时数据。";
        tool_hints = "\n\n使用 sys_info 获取系统状态。";
    }

    /* 组合提示词 */
    snprintf(out, out_size,
        "%s\n\n"
        "用户请求：\n%s\n\n"
        "请分析请求，如果需要实时系统数据，使用相应的 MCP 工具。\n"
        "返回结构化结果。%s",
        system_prompt, user_request, tool_hints);
}

/* =========================================================================
 * 主路由函数
 * ========================================================================= */

/*
 * skill_router_dispatch — Skill 路由分发
 *
 * 完整流程：
 *   1. 分析请求，确定 Skill 类别
 *   2. 执行 MCP 工具获取数据
 *   3. 路由选择 Backend
 *   4. 构建增强提示词
 *   5. 调用 Backend API
 *   6. 返回结果
 */
typedef struct {
    const char *user_request;
    char *api_result;
    size_t api_result_size;
    struct routing_decision decision;
    unsigned int category;
    char mcp_result[4096];
    int mcp_used;
} SkillDispatchCtx;

int skill_router_dispatch(SkillDispatchCtx *ctx)
{
    if (!ctx || !ctx->user_request) return -1;

    /* Step 1: 分析 Skill 类别 */
    ctx->category = skill_analyze(ctx->user_request);

    /* Step 2: 选择 MCP 工具并调用 */
    ctx->mcp_used = 0;
    ctx->mcp_result[0] = '\0';

    for (size_t i = 0; i < sizeof(tool_mappings)/sizeof(tool_mappings[0]); i++) {
        const SkillToolMapping *m = &tool_mappings[i];
        if (ctx->category & m->category) {
            char tool_result[1024];
            if (skill_execute_tool(m->mcp_tool, ctx->user_request,
                                   tool_result, sizeof(tool_result)) == 0) {
                strncat(ctx->mcp_result, m->prompt_suffix,
                       sizeof(ctx->mcp_result) - strlen(ctx->mcp_result) - 1);
                strncat(ctx->mcp_result, tool_result,
                       sizeof(ctx->mcp_result) - strlen(ctx->mcp_result) - 1);
                strncat(ctx->mcp_result, "\n",
                       sizeof(ctx->mcp_result) - strlen(ctx->mcp_result) - 1);
                ctx->mcp_used = 1;
                break; /* 每个 category 只取第一个匹配的 */
            }
        }
    }

    /* Step 3: 路由选择 Backend */
    skill_route(ctx->category, &ctx->decision);

    /* Step 4: 构建增强提示词 */
    char enhanced_prompt[8192];
    build_skill_prompt(enhanced_prompt, sizeof(enhanced_prompt),
                       ctx->user_request, ctx->category);

    /* 追加 MCP 工具结果 */
    if (ctx->mcp_used) {
        strncat(enhanced_prompt, "\n\n实时数据：\n",
               sizeof(enhanced_prompt) - strlen(enhanced_prompt) - 1);
        strncat(enhanced_prompt, ctx->mcp_result,
               sizeof(enhanced_prompt) - strlen(enhanced_prompt) - 1);
    }

    /* Step 5: 调用 Backend API（由调用方执行）*/
    /* 这里把增强提示词放入 ctx->api_result 作为输入 */
    if (ctx->api_result && ctx->api_result_size > 0) {
        strncpy(ctx->api_result, enhanced_prompt,
               ctx->api_result_size - 1);
        ctx->api_result[ctx->api_result_size - 1] = '\0';
    }

    return 0;
}

/* =========================================================================
 * 初始化
 * ========================================================================= */

void skill_router_init(void)
{
    skill_registry_init(); /* 注册所有工具和 Skill */
}

void skill_router_set_api_key(int backend, const char *key)
{
    if (backend == 0)
        strncpy(deepseek_cfg.api_key, key, sizeof(deepseek_cfg.api_key) - 1);
    else
        strncpy(kimi_cfg.api_key, key, sizeof(kimi_cfg.api_key) - 1);
}

void skill_router_set_weights(int backend,
                              int sched, int io, int sec,
                              int mem, int code, int orch)
{
    if (backend == 0) {
        deepseek_cfg.weight_sched = sched;
        deepseek_cfg.weight_io    = io;
        deepseek_cfg.weight_sec   = sec;
        deepseek_cfg.weight_mem   = mem;
        deepseek_cfg.weight_code  = code;
        deepseek_cfg.weight_orch  = orch;
    } else {
        kimi_cfg.weight_sched = sched;
        kimi_cfg.weight_io    = io;
        kimi_cfg.weight_sec   = sec;
        kimi_cfg.weight_mem   = mem;
        kimi_cfg.weight_code  = code;
        kimi_cfg.weight_orch  = orch;
    }
}
