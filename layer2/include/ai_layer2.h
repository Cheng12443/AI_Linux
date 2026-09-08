/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ai_layer2.h — Layer2 AI 网关通信协议
 *
 * 定义内核态 ↔ 用户态之间的双向通信协议：
 *   - netlink 传递系统上下文和推理结果
 *   - 共享内存传递大批量数据（避免反复 copy）
 *   - io_uring 用于高吞吐推理请求
 *
 * 协议版本：1.0
 * 版本号编码：major << 16 | minor
 */

#ifndef _AI_LAYER2_H
#define _AI_LAYER2_H

#include <linux/types.h>
#include <linux/socket.h>

#define AI_LAYER2_VERSION    0x00010000  /* v1.0.0 */
#define AI_LAYER2_FAMILY     "AI_LAYER2"
#define AI_LAYER2_MC_GROUP   "ai_layer2_events"

/* =========================================================================
 * netlink 属性类型
 * ========================================================================= */
enum ai_layer2_attr {
    AI_L2_UNSPEC = 0,

    /* ===== 路由信息：内核 → 用户态 ===== */
    AI_L2_KERNEL_TELEMETRY   = 1,  /* 系统遥测数据 */
    AI_L2_SCHED_CONTEXT      = 2,  /* 调度上下文（任务特征） */
    AI_L2_IO_CONTEXT         = 3,  /* IO 上下文（流量特征） */
    AI_L2_MEM_CONTEXT        = 4,  /* 内存上下文 */
    AI_L2_SECURITY_EVENT     = 5,  /* 安全事件 */
    AI_L2_MODEL_RESPONSE     = 6,  /* Layer1 推理结果（用于对比） */

    /* ===== 决策路由：用户态 → 内核 ===== */
    AI_L2_AI_DECISION        = 10, /* Layer2 决策结果 */
    AI_L2_CONFIG_UPDATE      = 11, /* 配置更新 */
    AI_L2_MODEL_DOWNLOAD     = 12, /* 模型下载请求 */
    AI_L2_ALERT              = 13, /* 告警 */

    /* ===== 控制 ===== */
    AI_L2_ROUTE_ENABLE       = 20, /* 启用/禁用路由 */
    AI_L2_HEARTBEAT          = 21, /* 心跳 */
    AI_L2_ACK                = 22, /* 确认 */
    AI_L2_ERROR              = 23, /* 错误报告 */

    AI_L2_MAX = 64,
};

/* =========================================================================
 * 消息类型（netlink 命令）
 * ========================================================================= */
enum ai_layer2_cmd {
    AI_L2_CMD_UNSPEC = 0,

    /* 内核 → 用户态 */
    AI_L2_CMD_TELEMETRY    = 1,  /* 推送系统遥测 */
    AI_L2_CMD_DECISION_REQ = 2,  /* 请求 Layer2 推理决策 */
    AI_L2_CMD_EVENT        = 3,  /* 安全/IO 事件通知 */
    AI_L2_CMD_STATS        = 4,  /* Layer1 统计上报 */
    AI_L2_CMD_MODEL_ACK    = 5,  /* 模型加载确认 */

    /* 用户态 → 内核 */
    AI_L2_CMD_DECISION_RES = 10, /* Layer2 推理结果 */
    AI_L2_CMD_CONFIG_SET  = 11, /* 设置配置 */
    AI_L2_CMD_ROUTE_CTRL  = 12, /* 路由控制 */
    AI_L2_CMD_MODEL_LOAD  = 13, /* 加载模型 */
    AI_L2_CMD_ALERT_ACK   = 14, /* 告警确认 */

    /* 双向 */
    AI_L2_CMD_HELLO        = 20, /* 建立连接 */
    AI_L2_CMD_GOODBYE      = 21, /* 断开连接 */
    AI_L2_CMD_PING         = 22, /* 心跳 */
    AI_L2_CMD_PONG         = 23, /* 心跳响应 */

    AI_L2_CMD_MAX,
};

/* =========================================================================
 * 核心数据结构（固定大小，便于 netlink 传输）
 * ========================================================================= */

/* 路由控制标志 */
#define AI_L2_F_ROUTING_ENABLED  (1 << 0)
#define AI_L2_F_LAYER1_FALLBACK  (1 << 1)  /* Layer1 作为降级路径 */
#define AI_L2_F_SYNC_MODE        (1 << 2)  /* 同步模式（等待 Layer2） */
#define AI_L2_F_ASYNC_MODE       (1 << 3)  /* 异步模式（Layer2 并行推理） */
#define AI_L2_F_DEBUG            (1 << 4)  /* 调试模式 */

/* 路由模式 */
enum ai_layer2_mode {
    AI_L2_MODE_DISABLED  = 0,   /* 路由关闭 */
    AI_L2_MODE_SYNC      = 1,   /* 同步：Layer1 先推理，Layer2 复核 */
    AI_L2_MODE_ASYNC     = 2,   /* 异步：Layer1 快速决策，Layer2 异步评估 */
    AI_L2_MODE_OVERRIDE  = 3,   /* Layer2 覆盖 Layer1 决策 */
    AI_L2_MODE_ORCHESTRA = 4,   /* 编排：Layer1 + Layer2 联合决策 */
};

/* 系统遥测数据（内核 → 用户态） */
struct ai_layer2_telemetry {
    __u64 timestamp_ns;

    /* CPU 统计 */
    __u32 cpu_util[64];         /* 每个 CPU 的利用率（0-1024） */
    __u32 cpu_num_online;
    __u32 cpu_num_total;
    __u64 sched_ctx_switches;   /* 上下文切换总数 */
    __u64 sched_irq_count;      /* 中断次数 */

    /* 内存统计 */
    __u64 mem_total_kb;
    __u64 mem_free_kb;
    __u64 mem_available_kb;
    __u64 swap_total_kb;
    __u64 swap_free_kb;
    __u64 anon_pages;
    __u64 file_pages;
    __u64 shmem_pages;

    /* IO 统计 */
    __u64 io_read_bytes;
    __u64 io_write_bytes;
    __u64 io_read_ops;
    __u64 io_write_ops;

    /* 网络统计（简化） */
    __u64 net_rx_bytes;
    __u64 net_tx_bytes;
    __u64 net_rx_dropped;
    __u64 net_tx_dropped;

    /* 安全统计 */
    __u32 sec_events_total;
    __u32 sec_events_blocked;
    __u32 sec_events_allowed;

    /* 进程信息 */
    __u32 num_processes;
    __u32 num_running;
    __u32 num_sleeping;
    __u32 num_zombie;
    __u32 num_threads;

    /* 负载均值 */
    __u32 loadavg_1;
    __u32 loadavg_5;
    __u32 loadavg_15;

    /* 温度（如果有） */
    __u32 thermal_temp;         /* 摄氏温度 × 1000 */

    /* AI 统计 */
    __u64 ai_inferences_l1;      /* Layer1 推理次数 */
    __u64 ai_inferences_l2;      /* Layer2 推理次数 */
    __u64 ai_latency_l1_ns;      /* Layer1 平均延迟 */
    __u64 ai_latency_l2_ns;      /* Layer2 平均延迟 */
    __u64 ai_errors;
    __u32 ai_confidence;         /* 全局置信度 0-10000 */
};

/* 调度上下文（内核 → 用户态） */
struct ai_layer2_sched_context {
    __u64 timestamp_ns;
    __u32 pid;
    __u32 tid;
    __u32 ppid;
    __u32 uid;
    __u32 gid;
    __u8  comm[64];
    __u8  exe_path[256];

    /* 资源使用 */
    __u64 cpu_time_ns;
    __u64 start_time_ns;
    __u64 vruntime_ns;
    __u32 cpu_util;             /* 0-1024 */
    __u32 cpu_affinity_mask;    /* 位掩码 */
    __u32 nice;
    __u32 prio;                 /* 动态优先级 */
    __u32 static_prio;
    __u32 normal_prio;

    /* 内存 */
    __u64 vm_size_kb;
    __u64 vm_rss_kb;
    __u64 vm_shared_kb;
    __u64 vm_executable_kb;
    __u64 vm_stack_kb;

    /* IO */
    __u64 io_read_bytes;
    __u64 io_write_bytes;
    __u32 io_class;             /* IOSCHED class */
    __u32 io_priority;

    /* 调度决策 */
    __u32 nvcsw;                /* 自愿切换 */
    __u32 nivcsw;               /* 非自愿切换 */
    __u32 last_cpu;
    __u32 last_sibling_cpu;
    __u8  is_foreground;
    __u8  is_batch;
    __u8  is_idle;
    __u8  is_stopped;
    __u8  oom_score_adj;

    /* Layer1 推理结果（供 Layer2 复核） */
    __u8  layer1_decision;      /* enum ai_sched_decision */
    __u32 layer1_score;        /* Layer1 置信度 × 10000 */
    __u64 layer1_latency_ns;
};

/* IO 上下文 */
struct ai_layer2_io_context {
    __u64 timestamp_ns;
    __u32 pid;
    __u32 uid;
    __u8  comm[64];

    /* 网络连接 */
    __u8  family;               /* AF_INET / AF_INET6 / ... */
    __u8  protocol;             /* IPPROTO_TCP / UDP / ... */
    __u16 src_port;
    __u16 dst_port;
    __u8  src_ip[16];
    __u8  dst_ip[16];
    __u8  state;                /* TCP state */
    __u32 rx_queue;
    __u32 tx_queue;

    /* 包特征 */
    __u32 packet_len;
    __u8  tcp_flags;
    __u8  dscp;
    __u8  ttl;
    __u8  ip_version;

    /* 流量统计 */
    __u64 rx_bytes;
    __u64 tx_bytes;
    __u64 rx_packets;
    __u64 tx_packets;

    /* Layer1 XDP 结果 */
    __u8  layer1_action;         /* XDP_PASS / DROP / REDIRECT */
    __u32 layer1_score;         /* Layer1 置信度 × 10000 */
};

/* Layer2 决策（用户态 → 内核） */
struct ai_layer2_decision {
    __u64 timestamp_ns;
    __u64 request_id;           /* 关联原始请求 */
    __u8  domain;               /* 0=sched, 1=io, 2=security, 3=memory */

    /* 决策内容 */
    __u8  decision;             /* 具体决策值 */
    __u32 confidence;           /* 置信度 × 10000 */
    __u32 score;                /* 模型输出分数 */

    /* 决策理由（可读字符串指针） */
    __u32 reason_len;
    __u8  reason[256];          /* JSON 格式的推理理由 */

    /* 模型信息 */
    __u8  model_name[64];
    __u8  model_provider[32];   /* openai / anthropic / local / ... */
    __u8  model_version[16];
    __u64 model_latency_ns;

    /* 覆盖标志 */
    __u8  override_layer1;      /* 是否覆盖 Layer1 决策 */
    __u8  confidence_threshold;/* 本次决策的阈值 */
    __u8  pad[2];
};

/* 配置更新 */
struct ai_layer2_config {
    __u32 mode;                  /* enum ai_layer2_mode */
    __u32 flags;                 /* AI_L2_F_* */
    __u32 sync_timeout_ms;       /* 同步模式超时 */
    __u32 async_queue_depth;      /* 异步队列深度 */
    __u32 max_batch_size;        /* 最大批量大小 */
    __u32 batch_timeout_ms;      /* 批量超时 */

    /* API 配置 */
    __u8  api_provider[32];       /* openai / anthropic / azure / gemini */
    __u8  api_endpoint[256];      /* API URL */
    __u8  api_model[64];          /* 模型名 */
    __u8  api_key_env[64];        /* 环境变量名（不直接传 key）*/
    __u32 api_timeout_ms;
    __u32 api_max_retries;
    __u32 api_rate_limit;         /* 每分钟请求数 */

    /* 路由规则 */
    __u32 route_sched_threshold;  /* 调度路由置信度阈值 */
    __u32 route_io_threshold;
    __u32 route_sec_threshold;
    __u32 route_mem_threshold;

    __u8  pad[4];
};

/* 告警 */
struct ai_layer2_alert {
    __u64 timestamp_ns;
    __u32 alert_id;
    __u8  severity;              /* 0=info, 1=warning, 2=error, 3=critical */
    __u8  domain;                /* 0=sched, 1=io, 2=sec, 3=mem, 4=system */
    __u8  action_required;
    __u8  pad;
    __u32 pid;
    __u8  message[256];
    __u8  recommendation[256];
};

/* =========================================================================
 * 共享内存协议（用于大批量数据）
 * ========================================================================= */

/* 共享内存区域布局 */
struct ai_layer2_shmem_header {
    __u32 magic;           /* 魔数：0x41494C32 ('AIL2') */
    __u32 version;
    __u32 size;            /* 总大小 */
    __u32 num_slots;       /* 槽位数 */
    __u64 shmem_addr;      /* 实际地址 */
    __u32 refcnt;
    __u32 padding;

    /* 原子操作索引 */
    atomic_t producer_idx;
    atomic_t consumer_idx;
    atomic_t slot_inuse;
};

/* 共享内存槽 */
struct ai_layer2_shmem_slot {
    __u64 slot_id;
    __u32 size;
    __u32 flags;
    __u64 timestamp_ns;
    __u32 seq;             /* 序列号 */
    __u8  type;            /* 数据类型 */
    __u8  pad[7];
    /* 数据紧跟其后 */
};

/* 槽类型 */
enum ai_layer2_slot_type {
    AI_L2_SLOT_TELEMETRY  = 1,
    AI_L2_SLOT_SCHED      = 2,
    AI_L2_SLOT_IO         = 3,
    AI_L2_SLOT_DECISION   = 10,
    AI_L2_SLOT_MODEL_DATA = 20,
};

/* =========================================================================
 * io_uring 协议（高吞吐推理）
 * ========================================================================= */

/* 推理请求（通过 io_uring 共享） */
struct ai_layer2_inference_req {
    __u64  request_id;
    __u64  timestamp_ns;
    __u32  domain;        /* 0=sched, 1=io, 2=sec, 3=mem */
    __u32  flags;

    /* 输入数据（指向共享内存） */
    __u64  input_shmem_offset;  /* 共享内存偏移 */
    __u32  input_size;

    /* 输出数据 */
    __u64  output_shmem_offset;
    __u32  output_size;

    /* 优先级 */
    __u8   priority;     /* 0=low, 1=normal, 2=high */
    __u8   pad[7];

    /* 回调信息 */
    __u64  user_data;    /* 调用者私有数据 */
};

/* 推理结果 */
struct ai_layer2_inference_res {
    __u64  request_id;
    __u64  timestamp_ns;
    __u32  err;

    /* 决策 */
    __u8   decision;
    __u8   confidence;   /* 0-100 */
    __u8   model_id[64];
    __u8   reason[512];  /* JSON */

    __u64  latency_ns;
};

/* =========================================================================
 * 内核端 API（供 ai_core 调用）
 * ========================================================================= */

/* 初始化 Layer2 路由 */
int ai_layer2_init(void);
void ai_layer2_exit(void);

/* 推送遥测数据 */
int ai_layer2_push_telemetry(struct ai_layer2_telemetry *telem);

/* 推送调度上下文（请求 Layer2 决策） */
int ai_layer2_request_decision(struct ai_layer2_sched_context *ctx,
                                void (*callback)(struct ai_layer2_decision *, void *),
                                void *priv, unsigned long timeout_ms);

/* 推送 IO 上下文 */
int ai_layer2_push_io_context(struct ai_layer2_io_context *io_ctx);

/* 推送安全事件 */
int ai_layer2_push_security_event(struct ai_layer2_alert *alert);

/* 设置配置 */
int ai_layer2_set_config(struct ai_layer2_config *cfg);

/* 获取配置 */
int ai_layer2_get_config(struct ai_layer2_config *cfg);

/* 统计 */
void ai_layer2_stats(u64 *sent, u64 *received, u64 *errors, u64 *dropped);

/* 路由状态 */
int ai_layer2_get_routing_state(void); /* 返回当前路由模式 */

/* =========================================================================
 * proc 接口
 * ========================================================================= */
void ai_layer2_proc_init(void);
void ai_layer2_proc_exit(void);

/* =========================================================================
 * 工具函数
 * ========================================================================= */

/* 决策置信度映射到可读字符串 */
static inline const char *
ai_layer2_confidence_str(__u32 confidence)
{
    if (confidence >= 9000) return "极高";
    if (confidence >= 7000) return "高";
    if (confidence >= 5000) return "中";
    if (confidence >= 3000) return "低";
    return "极低";
}

/* 版本比较 */
static inline int
ai_layer2_check_version(__u32 version)
{
    return version >= AI_LAYER2_VERSION ? 0 : -EPROTO;
}

#endif /* _AI_LAYER2_H */
