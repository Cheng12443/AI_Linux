/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ai_mcp.h — MCP (Model Context Protocol) 核心定义
 *
 * 支持 MCP 核心子集：
 *   - JSON-RPC 2.0 消息格式
 *   - tools/list, tools/call
 *   - resources/list, resources/read
 */

#ifndef _AI_MCP_H
#define _AI_MCP_H

#include <stdint.h>

#ifndef _AI_LIST_H
#define _AI_LIST_H
/* 用户态简化版 list_head */
struct list_head {
    struct list_head *next, *prev;
};
static inline void list_add(struct list_head *n, struct list_head *h) {
    n->next = h->next; n->prev = h;
    h->next->prev = n; h->next = n;
}
static inline void list_del(struct list_head *n) {
    n->prev->next = n->next; n->next->prev = n->prev;
}
static inline int list_empty(struct list_head *h) { return h->next == h; }
#define list_for_each_entry(pos, head, member) \
    for (pos = (typeof(pos))((head)->next); \
         &pos->member != (head); \
         pos = (typeof(pos))(pos->member.next))
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
#endif

#define MCP_VERSION        "2024-11-05"
#define MCP_PROTOCOL       "io.mcp"
#define MCP_MAX_TOOLS      256
#define MCP_MAX_PARAMS     32

/* =========================================================================
 * JSON-RPC 2.0 消息类型
 * ========================================================================= */

#define MCP_METHOD_TOOLS_LIST     "tools/list"
#define MCP_METHOD_TOOLS_CALL    "tools/call"
#define MCP_METHOD_RESOURCES_LIST "resources/list"
#define MCP_METHOD_RESOURCES_READ "resources/read"
#define MCP_METHOD_INITIALIZE    "initialize"
#define MCP_METHOD_SHUTDOWN      "shutdown"
#define MCP_METHOD_COMPLETE      "completion/complete"

#define MCP_NOTIFICATION_INITIALIZED "notifications/initialized"

/* =========================================================================
 * 工具参数
 * ========================================================================= */

struct mcp_param {
    char     name[64];
    char     type[32];       /* string / number / boolean / integer / object */
    char     description[256];
    int      required;        /* 1=必填，0=可选 */
    char     default_val[128];
};

struct mcp_tool {
    char     name[64];
    char     description[512];
    char     category[32];   /* scheduling / io / security / memory / system */

    struct mcp_param params[MCP_MAX_PARAMS];
    int              num_params;

    void            *handler;  /* 函数指针 */
    void            *priv;

    struct list_head node;
};

/* =========================================================================
 * 工具调用结果
 * ========================================================================= */

struct mcp_call_result {
    int       success;
    char      content[4096]; /* JSON 格式返回 */
    char      error[512];
    int       is_error;
};

/* =========================================================================
 * MCP 消息（JSON-RPC 2.0）
 * ========================================================================= */

struct mcp_message {
    char     jsonrpc[16];     /* "2.0" */
    unsigned int id;          /* 请求 ID */

    char     method[128];

    /* params: JSON 对象字符串 */
    char     params[2048];

    /* result: JSON 格式结果 */
    char     result[4096];

    /* error */
    int      error_code;
    char     error_message[512];

    unsigned char is_notification;
    unsigned char is_response;
};

/* =========================================================================
 * Skill 路由
 * ========================================================================= */

#define SKILL_CAT_SCHEDULING   0x01
#define SKILL_CAT_IO_NETWORK  0x02
#define SKILL_CAT_SECURITY    0x04
#define SKILL_CAT_MEMORY      0x08
#define SKILL_CAT_SYSTEM      0x10
#define SKILL_CAT_CODE        0x20
#define SKILL_CAT_ANALYSIS    0x40
#define SKILL_CAT_ORCHESTRATE 0x80

#define BACKEND_CAP_REASONING   0x01
#define BACKEND_CAP_CODING      0x02
#define BACKEND_CAP_LONG_CTX    0x04
#define BACKEND_CAP_FAST        0x08
#define BACKEND_CAP_MULTIMODAL  0x20

struct skill {
    char     name[64];
    char     description[256];
    unsigned int category;
    unsigned int required_caps;
    char     mcp_tool_name[64];
    char     system_prompt[1024];
    char     user_template[1024];
    int      deepseek_weight;
    int      kimi_weight;
    struct list_head node;
};

struct routing_decision {
    int      backend;         /* 0=DeepSeek, 1=Kimi */
    char     model[64];
    int      confidence;
    char     reason[256];
    unsigned int caps;
};

#endif /* _AI_MCP_H */
