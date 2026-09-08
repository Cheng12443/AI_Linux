// SPDX-License-Identifier: GPL-2.0
/*
 * ai_mcp_core.c — MCP 核心实现
 *
 * 功能：
 *   - JSON-RPC 2.0 解析/序列化
 *   - 工具注册与调用
 *   - 消息路由
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pthread.h>

#include "ai_mcp.h"

/* =========================================================================
 * 全局数据
 * ========================================================================= */

static LIST_HEAD(mcp_tools);
static LIST_HEAD(skills);
static pthread_mutex_t mcp_lock = PTHREAD_MUTEX_INITIALIZER;
static int next_id = 1;

/* =========================================================================
 * JSON-RPC 2.0 解析
 * ========================================================================= */

/* 跳过空白 */
static const char *json_skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* 解析字符串（带引号）*/
static int json_parse_string(const char *p, char *out, size_t out_size)
{
    if (*p != '"') return -1;
    p++;
    char *op = out;
    size_t left = out_size - 1;

    while (*p && *p != '"' && left > 1) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  *op++ = '"';  left--; break;
            case '\\': *op++ = '\\'; left--; break;
            case 'n':  *op++ = '\n'; left--; break;
            case 'r':  *op++ = '\r'; left--; break;
            case 't':  *op++ = '\t'; left--; break;
            default:   *op++ = *p;  left--; break;
            }
            p++;
        } else {
            *op++ = *p++;
            left--;
        }
    }
    *op = '\0';
    if (*p == '"') p++;
    return 0;
}

/* 解析整数 */
static long json_parse_int(const char *p)
{
    return strtol(p, NULL, 10);
}

/* 提取字段（简单版）*/
static int json_get_str(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        json_parse_string(p + 1, out, out_size);
        return 0;
    }
    return -1;
}

/* 提取整数字段 */
static int json_get_int_val(const char *json, const char *key)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p && !isdigit((unsigned char)*p) && *p != '-') p++;
    return atoi(p);
}

/* =========================================================================
 * JSON-RPC 2.0 序列化
 * ========================================================================= */

static int jsonrpc_build_response(char *out, size_t out_size,
                                 int id,
                                 const char *result_json)
{
    return snprintf(out, out_size,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}",
        id, result_json);
}

static int jsonrpc_build_error(char *out, size_t out_size,
                               int id, int code, const char *message)
{
    return snprintf(out, out_size,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
        id, code, message);
}

/* 构建 tools/list 响应 */
static int mcp_build_tools_list(char *out, size_t out_size)
{
    char *p = out;
    size_t left = out_size;

    p += snprintf(p, left, "{\"tools\":[");
    size_t pos = p - out;

    pthread_mutex_lock(&mcp_lock);
    struct mcp_tool *t;
    int first = 1;
    list_for_each_entry(t, &mcp_tools, node) {
        if (!first) {
            p += snprintf(p, left, ",");
            left = out_size - (p - out);
        }
        p += snprintf(p, left,
            "{\"name\":\"%s\",\"description\":\"%s\",\"inputSchema\":{",
            t->name, t->description);
        left = out_size - (p - out);

        if (t->num_params > 0) {
            p += snprintf(p, left, "\"type\":\"object\",\"properties\":{");
            left = out_size - (p - out);

            for (int i = 0; i < t->num_params; i++) {
                struct mcp_param *pr = &t->params[i];
                if (i > 0) {
                    p += snprintf(p, left, ",");
                    left = out_size - (p - out);
                }
                p += snprintf(p, left,
                    "\"%s\":{\"type\":\"%s\",\"description\":\"%s\"}",
                    pr->name, pr->type, pr->description);
                left = out_size - (p - out);
            }
            p += snprintf(p, left, "}}");
            left = out_size - (p - out);
        } else {
            p += snprintf(p, left, "\"type\":\"object\",\"properties\":{}}");
            left = out_size - (p - out);
        }

        p += snprintf(p, left, "}");
        left = out_size - (p - out);
        first = 0;
    }
    pthread_mutex_unlock(&mcp_lock);

    p += snprintf(p, left, "],\"_meta\":{\"version\":\"%s\"}}", MCP_VERSION);

    return p - out;
}

/* 构建 resources/list 响应 */
static int mcp_build_resources_list(char *out, size_t out_size)
{
    return snprintf(out, out_size,
        "{\"resources\":["
        "{\"uri\":\"kernel://telemetry\",\"name\":\"系统遥测\",\"mimeType\":\"application/json\"},"
        "{\"uri\":\"kernel://sched\",\"name\":\"调度状态\",\"mimeType\":\"application/json\"},"
        "{\"uri\":\"kernel://io\",\"name\":\"IO状态\",\"mimeType\":\"application/json\"},"
        "{\"uri\":\"kernel://memory\",\"name\":\"内存状态\",\"mimeType\":\"application/json\"},"
        "{\"uri\":\"kernel://stats\",\"name\":\"AI统计\",\"mimeType\":\"application/json\"}"
        "],\"_meta\":{\"version\":\"%s\"}}",
        MCP_VERSION);
}

/* =========================================================================
 * 工具注册
 * ========================================================================= */

void mcp_tool_register(struct mcp_tool *tool)
{
    pthread_mutex_lock(&mcp_lock);
    list_add(&tool->node, &mcp_tools);
    pthread_mutex_unlock(&mcp_lock);
}

void mcp_tool_unregister(struct mcp_tool *tool)
{
    pthread_mutex_lock(&mcp_lock);
    list_del(&tool->node);
    pthread_mutex_unlock(&mcp_lock);
}

/* =========================================================================
 * 工具调用
 * ========================================================================= */

static int mcp_call_tool(const char *tool_name,
                         struct mcp_call_result *result)
{
    /* 权限检查：只允许已注册的工具 */
    /* 真实场景应检查调用者 capability */
    (void)capable; /* suppress warning */

    /* 防止路径遍历 */
    if (strstr(tool_name, "/") || strstr(tool_name, "..")) {
        result->success = 0;
        result->is_error = 1;
        snprintf(result->error, sizeof(result->error),
                 "invalid tool name");
        return -1;
    }

    (void)params_json;

    memset(result, 0, sizeof(*result));

    pthread_mutex_lock(&mcp_lock);
    struct mcp_tool *t;
    int found = 0;

    list_for_each_entry(t, &mcp_tools, node) {
        if (strcmp(t->name, tool_name) == 0) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&mcp_lock);

    if (!found) {
        result->success = 0;
        result->is_error = 1;
        snprintf(result->error, sizeof(result->error),
                 "tool '%s' not found", tool_name);
        return -1;
    }

    /* 调用处理函数 */
    if (t->handler) {
        typedef int (*tool_handler_t)(const char *params, char *result, size_t size, void *priv);
        tool_handler_t fn = (tool_handler_t)t->handler;
        int ret = fn(params_json, result->content, sizeof(result->content), t->priv);
        result->success = (ret == 0);
        if (!result->success) {
            result->is_error = 1;
            snprintf(result->error, sizeof(result->error), "tool execution failed");
        }
    } else {
        result->success = 1;
        snprintf(result->content, sizeof(result->content),
                 "{\"status\":\"ok\",\"tool\":\"%s\"}", tool_name);
    }

    return result->success ? 0 : -1;
}

/* =========================================================================
 * MCP 消息处理
 * ========================================================================= */

/*
 * mcp_handle_message — 处理接收到的 MCP 消息
 *
 * 输入：JSON-RPC 2.0 字符串
 * 输出：响应 JSON-RPC 2.0 字符串
 *
 * 返回写入的字节数
 */
int mcp_handle_message(const char *input,
                       char *output, size_t out_size)
{
    memset(output, 0, out_size);

    const char *p = json_skip_ws(input);
    if (strncmp(p, "{\"jsonrpc", 9) != 0) {
        return jsonrpc_build_error(output, out_size, 0, -32700, "invalid request");
    }

    /* 提取 id */
    const char *id_p = strstr(p, "\"id\"");
    int msg_id = 0;
    if (id_p) {
        id_p += 4;
        while (*id_p && !isdigit((unsigned char)*id_p)) id_p++;
        msg_id = atoi(id_p);
    }

    /* 提取 method */
    char method[128];
    if (json_get_str(p, "method", method, sizeof(method)) < 0) {
        return jsonrpc_build_error(output, out_size, msg_id, -32600, "method not found");
    }

    /* 提取 params */
    char params[2048] = { 0 };
    const char *params_p = strstr(p, "\"params\"");
    if (params_p) {
        params_p += 8;
        while (*params_p && *params_p != '{' && *params_p != '[') params_p++;
        /* 找匹配的闭合括号 */
        if (*params_p == '{' || *params_p == '[') {
            char *out = params;
            size_t left = sizeof(params) - 1;
            int depth = 0;
            int started = 0;
            while (*params_p && left > 1) {
                if ((*params_p == '{' || *params_p == '[') && !started) {
                    started = 1;
                    depth = 1;
                    *out++ = *params_p++;
                    left--;
                } else if (started) {
                    if (*params_p == '{' || *params_p == '[') depth++;
                    else if (*params_p == '}' || *params_p == ']') depth--;
                    *out++ = *params_p++;
                    left--;
                    if (depth == 0) break;
                } else {
                    params_p++;
                }
            }
            *out = '\0';
        }
    }

    /* 方法分发 */
    char result_json[8192] = { 0 };

    if (strcmp(method, MCP_METHOD_TOOLS_LIST) == 0) {
        mcp_build_tools_list(result_json, sizeof(result_json));

    } else if (strcmp(method, MCP_METHOD_TOOLS_CALL) == 0) {
        char tool_name[64];
        if (json_get_str(params, "name", tool_name, sizeof(tool_name)) < 0) {
            return jsonrpc_build_error(output, out_size, msg_id, -32602,
                                      "missing 'name' parameter");
        }

        char tool_args[2048];
        if (json_get_str(params, "arguments", tool_args, sizeof(tool_args)) < 0) {
            tool_args[0] = '\0';
        }

        struct mcp_call_result cr;
        int ret = mcp_call_tool(tool_name, tool_args, &cr);

        if (ret == 0) {
            snprintf(result_json, sizeof(result_json),
                    "{\"content\":[{\"type\":\"text\",\"text\":%s}],\"isError\":false}",
                    cr.content);
        } else {
            snprintf(result_json, sizeof(result_json),
                    "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":true}",
                    cr.error);
        }

    } else if (strcmp(method, MCP_METHOD_RESOURCES_LIST) == 0) {
        mcp_build_resources_list(result_json, sizeof(result_json));

    } else if (strcmp(method, MCP_METHOD_RESOURCES_READ) == 0) {
        char uri[256];
        if (json_get_str(params, "uri", uri, sizeof(uri)) < 0) {
            return jsonrpc_build_error(output, out_size, msg_id, -32602,
                                      "missing 'uri' parameter");
        }

        /* 读取资源（需接入内核数据）*/
        snprintf(result_json, sizeof(result_json),
                "{\"contents\":[{\"uri\":\"%s\",\"mimeType\":\"application/json\","
                "\"text\":\"{\\\"data\\\":\\\"mock_resource_data\\\"}\"}]}",
                uri);

    } else if (strcmp(method, MCP_METHOD_INITIALIZE) == 0) {
        snprintf(result_json, sizeof(result_json),
                "{\"protocolVersion\":\"%s\",\"capabilities\":"
                "{\"tools\":{\"listChanged\":true},\"resources\":{\"subscribe\":false}},\"serverInfo\":"
                "{\"name\":\"ai-layer2\",\"version\":\"1.0.0\"}}",
                MCP_VERSION);

    } else if (strcmp(method, MCP_METHOD_SHUTDOWN) == 0) {
        return jsonrpc_build_response(output, out_size, msg_id, "{\"status\":\"shutdown\"}");

    } else {
        return jsonrpc_build_error(output, out_size, msg_id, -32601,
                                  "method not found");
    }

    return jsonrpc_build_response(output, out_size, msg_id, result_json);
}

/* =========================================================================
 * Skill 路由
 * ========================================================================= */

void skill_register(struct skill *s)
{
    pthread_mutex_lock(&mcp_lock);
    list_add(&s->node, &skills);
    pthread_mutex_unlock(&mcp_lock);
}

/*
 * skill_route — 根据任务类型选择最优 backend
 *
 * 输入：category（任务类型）、提示词特征
 * 输出：routing_decision（选择 DeepSeek 或 Kimi）
 */
int skill_route(unsigned int category, const char *prompt_hint,
                struct routing_decision *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    /* 默认各 50% */
    int deepseek_score = 50;
    int kimi_score = 50;

    /* 根据 category 调整权重 */
    if (category & SKILL_CAT_CODE) {
        /* 代码任务：DeepSeek coder 更强 */
        deepseek_score += 30;
        kimi_score -= 10;
    }

    if (category & SKILL_CAT_SCHEDULING) {
        /* 调度任务：两者皆可，DeepSeek reasoner 适合 */
        deepseek_score += 20;
        kimi_score += 10;
    }

    if (category & SKILL_CAT_SECURITY) {
        /* 安全任务：高置信度优先，Kimi 适合 */
        deepseek_score += 10;
        kimi_score += 20;
    }

    if (category & SKILL_CAT_MEMORY) {
        /* 内存任务：DeepSeek */
        deepseek_score += 15;
        kimi_score += 5;
    }

    if (category & SKILL_CAT_ORCHESTRATE) {
        /* 编排任务：Kimi 长上下文更强 */
        deepseek_score += 5;
        kimi_score += 25;
    }

    /* 边界裁剪 */
    if (deepseek_score > 100) deepseek_score = 100;
    if (kimi_score > 100)     kimi_score = 100;
    if (deepseek_score < 0)   deepseek_score = 0;
    if (kimi_score < 0)       kimi_score = 0;

    /* 选择 */
    if (deepseek_score >= kimi_score) {
        out->backend = 0; /* DeepSeek */
        strcpy(out->model, "deepseek-chat");
        out->confidence = deepseek_score;
        out->caps = BACKEND_CAP_FAST | BACKEND_CAP_CODING;
        snprintf(out->reason, sizeof(out->reason),
                 "deepseek_score=%d kimi_score=%d", deepseek_score, kimi_score);
    } else {
        out->backend = 1; /* Kimi */
        strcpy(out->model, "moonshot-v1-8k");
        out->confidence = kimi_score;
        out->caps = BACKEND_CAP_LONG_CTX | BACKEND_CAP_MULTIMODAL;
        snprintf(out->reason, sizeof(out->reason),
                 "kimi_score=%d deepseek_score=%d", kimi_score, deepseek_score);
    }

    /* 检查 category 特定模型 */
    if (category & SKILL_CAT_CODE) {
        strcpy(out->model, "deepseek-coder");
        out->caps |= BACKEND_CAP_CODING;
    }
    if (category & SKILL_CAT_ORCHESTRATE) {
        strcpy(out->model, "moonshot-v1-32k");
    }

    return 0;
}

/* 获取所有已注册的 skill */
int skill_list_all(struct skill **out_array, int max)
{
    int count = 0;
    pthread_mutex_lock(&mcp_lock);
    struct skill *s;
    list_for_each_entry(s, &skills, node) {
        if (count < max)
            out_array[count++] = s;
    }
    pthread_mutex_unlock(&mcp_lock);
    return count;
}
