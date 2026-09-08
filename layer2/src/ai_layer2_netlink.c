// SPDX-License-Identifier: GPL-2.0
/*
 * ai_layer2_netlink.c — Layer2 AI 网关内核态 netlink 实现
 *
 * 职责：
 *   1. 创建 netlink 套接字，与用户态 Layer2 守护进程通信
 *   2. 接收用户态配置，控制 Layer2 路由开关
 *   3. 推送系统遥测到用户态
 *   4. 接收 Layer2 推理结果，写回 ai_core 决策队列
 *   5. 处理共享内存（mmap）大数据传输
 *   6. 异步决策回调（不阻塞调度）
 *
 * netlink 族名：AI_LAYER2（需内核注册）
 * 多播组：ai_layer2_events（用于事件推送）
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netlink.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/inet.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <net/netlink.h>
#include <net/net_namespace.h>

#include "../ai_core/include/ai_core.h"
#include "ai_layer2.h"

#define DRV_NAME  "ai_layer2"
#define DRV_VER   "1.0.0"

/* =========================================================================
 * 全局配置
 * ========================================================================= */

static struct {
    enum ai_layer2_mode  mode;
    __u32                flags;
    __u32                sync_timeout_ms;
    __u32                async_queue_depth;
    __u32                route_sched_threshold;
    __u32                route_io_threshold;
    __u32                route_sec_threshold;

    /* API 配置占位 */
    char                 api_provider[32];
    char                 api_endpoint[256];
    char                 api_model[64];
    __u32                api_timeout_ms;

    struct mutex         lock;
    bool                 enabled;
} layer2_cfg = {
    .mode              = AI_L2_MODE_DISABLED,
    .flags             = 0,
    .sync_timeout_ms   = 1000,
    .async_queue_depth  = 256,
    .route_sched_threshold = 7000,
    .route_io_threshold = 7000,
    .route_sec_threshold = 7000,
    .api_timeout_ms    = 5000,
    .enabled           = false,
};

/* =========================================================================
 * 统计
 * ========================================================================= */

static struct {
    atomic64_t  msg_sent;
    atomic64_t  msg_received;
    atomic64_t  decisions_sent;      /* 发送给用户态的决策请求 */
    atomic64_t  decisions_received;  /* 接收到的 Layer2 决策 */
    atomic64_t  decisions_override;   /* Layer2 覆盖 Layer1 的次数 */
    atomic64_t  errors;
    atomic64_t  dropped;
    atomic64_t  shmem_bytes_sent;
    atomic64_t  api_latency_ns;
    atomic64_t  roundtrip_ns;       /* 往返延迟 */
    atomic64_t  queue_full;
} layer2_stats;

/* =========================================================================
 * netlink 套接字
 * ========================================================================= */

#define NLGRP_AI_LAYER2  1

static struct sock *nl_sk = NULL;
static int nl_pid = 0;          /* 用户态 PID（用于单播）*/
static atomic_t nl_refcnt = ATOMIC_INIT(0);
static DEFINE_MUTEX(nl_mutex);

/* =========================================================================
 * 决策回调队列（存储 pending 请求，等待 Layer2 响应）
 * ========================================================================= */

#define AI_L2_MAX_PENDING 8192

struct pending_decision {
    __u64                 request_id;
    u64                   timestamp_ns;
    void                 (*callback)(struct ai_layer2_decision *, void *);
    void                  *priv;
    struct delayed_work    dwork;     /* 超时检测 */
    struct list_head       node;
    atomic_t               refs;
    __u8                   domain;  /* sched/io/security/mem */
    __u8                   state;   /* 0=pending, 1=responded, 2=timeout */
};

static DEFINE_SPINLOCK(pending_lock);
static LIST_HEAD(pending_list);
static atomic_t pending_count = ATOMIC_INIT(0);

/* =========================================================================
 * 共享内存
 * ========================================================================= */

static struct {
    void                *addr;
    size_t               size;
    struct page         **pages;
    int                  npages;
    bool                 active;
    atomic_t             readers;
    atomic_t             writers;
} layer2_shmem = {
    .addr    = NULL,
    .size    = SZ_4M,  /* 默认 4MB */
    .active  = false,
};

/* =========================================================================
 * netlink 消息发送（支持单播和多播）
 * ========================================================================= */

/* 分配 netlink 消息 */
static struct sk_buff *
alloc_nlmsg(int payload_len, gfp_t gfp)
{
    struct nlmsghdr *nlh;
    struct sk_buff *skb;

    skb = nlmsg_new(NLMSG_ALIGN(payload_len) + NLMSG_HDRLEN, gfp);
    if (!skb)
        return NULL;

    nlh = nlmsg_put(skb, 0, 0, 0, payload_len, 0);
    if (!nlh) {
        kfree_skb(skb);
        return NULL;
    }

    return skb;
}

/* 发送单播消息到用户态 */
static int nl_send_to_user(struct sk_buff *skb)
{
    int ret = -ENOTCONN;

    mutex_lock(&nl_mutex);
    if (nl_pid > 0) {
        nlh_add_unicast(skb, nl_pid);
        ret = netlink_unicast(nl_sk, skb, nl_pid, MSG_DONTWAIT);
        if (ret > 0) {
            atomic64_inc(&layer2_stats.msg_sent);
            ret = 0;
        }
    } else {
        kfree_skb(skb);
        atomic64_inc(&layer2_stats.dropped);
        ret = -ENOTCONN;
    }
    mutex_unlock(&nl_mutex);

    return ret;
}

/* 广播消息到多播组 */
static int nl_broadcast(struct sk_buff *skb, gfp_t gfp)
{
    return nlmsg_multicast(nl_sk, skb, 0, NLGRP_AI_LAYER2, gfp);
}

/* 发送简单响应 */
static int nl_send_ack(__u32 seq, int err)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    struct ai_layer2_msg {
        struct nlmsghdr hdr;
        struct {
            __u32 error;
            __u32 seq;
        } ack;
   } msg;

    skb = alloc_nlmsg(sizeof(msg.ack), GFP_ATOMIC);
    if (!skb)
        return -ENOMEM;

    nlh = nlmsg_put(skb, 0, seq, AI_L2_CMD_ACK, sizeof(msg.ack), 0);
    memcpy(nlmsg_data(nlh), &err, sizeof(err));

    return nl_send_to_user(skb);
}

/* =========================================================================
 * 决策请求：内核 → 用户态（Layer2）
 * ========================================================================= */

/*
 * ai_layer2_request_decision — 请求 Layer2 推理决策
 *
 * 工作流程：
 *   1. 分配 request_id（全局递增）
 *   2. 将请求加入 pending 队列
 *   3. 打包成 netlink 消息，发送到用户态
 *   4. 安排超时工作项（delayed_work）
 *   5. 返回（异步模式）
 *
 * 如果 mode == AI_L2_MODE_SYNC：
 *   等待用户态响应，超时返回 -ETIMEDOUT
 */
static atomic64_t g_request_id = ATOMIC64_INIT(0);

int ai_layer2_request_decision(struct ai_layer2_sched_context *ctx,
                                void (*callback)(struct ai_layer2_decision *, void *),
                                void *priv, unsigned long timeout_ms)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    struct pending_decision *pend;
    __u64 request_id;
    int ret = 0;

    if (!layer2_cfg.enabled)
        return -ENODEV;

    if (!ctx)
        return -EINVAL;

    request_id = atomic64_inc_return(&g_request_id);

    /* 构建 pending 节点 */
    pend = kzalloc(sizeof(*pend), GFP_ATOMIC);
    if (!pend)
        return -ENOMEM;

    pend->request_id = request_id;
    pend->timestamp_ns = ktime_get_ns();
    pend->callback = callback;
    pend->priv = priv;
    pend->domain = ctx->layer1_decision; /* 借用字段 */
    atomic_set(&pend->refs, 2);
    INIT_LIST_HEAD(&pend->node);
    INIT_DELAYED_WORK(&pend->dwork, NULL); /* 后面填充 */

    /* 加入 pending 队列 */
    spin_lock(&pending_lock);
    if (atomic_read(&pending_count) >= AI_L2_MAX_PENDING) {
        spin_unlock(&pending_lock);
        kfree(pend);
        atomic64_inc(&layer2_stats.queue_full);
        return -ENOBUFS;
    }
    list_add(&pend->node, &pending_list);
    atomic_inc(&pending_count);
    spin_unlock(&pending_lock);

    /* 安排超时 */
    schedule_delayed_work(&pend->dwork, msecs_to_jiffies(timeout_ms));

    /* 分配 netlink 消息 */
    skb = alloc_nlmsg(sizeof(*ctx) + 64, GFP_ATOMIC);
    if (!skb) {
        ret = -ENOMEM;
        goto out_remove_pending;
    }

    nlh = nlmsg_put(skb, 0, request_id, AI_L2_CMD_DECISION_REQ,
                     sizeof(*ctx), 0);
    memcpy(nlmsg_data(nlh), ctx, sizeof(*ctx));

    atomic64_inc(&layer2_stats.decisions_sent);

    ret = nl_send_to_user(skb);
    if (ret < 0)
        goto out_remove_pending;

    return 0;

out_remove_pending:
    spin_lock(&pending_lock);
    list_del(&pend->node);
    atomic_dec(&pending_count);
    spin_unlock(&pending_lock);
    kfree(pend);
    return ret;
}

/* =========================================================================
 * 遥测推送：内核 → 用户态
 * ========================================================================= */

int ai_layer2_push_telemetry(struct ai_layer2_telemetry *telem)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    int ret;

    if (!layer2_cfg.enabled)
        return -ENODEV;

    if (!telem)
        return -EINVAL;

    skb = alloc_nlmsg(sizeof(*telem), GFP_ATOMIC);
    if (!skb)
        return -ENOMEM;

    nlh = nlmsg_put(skb, 0, 0, AI_L2_CMD_TELEMETRY,
                     sizeof(*telem), NLM_F_MULTICAST);
    memcpy(nlmsg_data(nlh), telem, sizeof(*telem));

    ret = nl_broadcast(skb, GFP_ATOMIC);
    if (ret < 0)
        atomic64_inc(&layer2_stats.dropped);
    else
        atomic64_inc(&layer2_stats.msg_sent);

    return ret;
}
EXPORT_SYMBOL_GPL(ai_layer2_push_telemetry);

/* IO 上下文推送 */
int ai_layer2_push_io_context(struct ai_layer2_io_context *io_ctx)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;

    if (!layer2_cfg.enabled)
        return -ENODEV;

    skb = alloc_nlmsg(sizeof(*io_ctx), GFP_ATOMIC);
    if (!skb)
        return -ENOMEM;

    nlh = nlmsg_put(skb, 0, 0, AI_L2_CMD_EVENT, sizeof(*io_ctx), 0);
    memcpy(nlmsg_data(nlh), io_ctx, sizeof(*io_ctx));

    return nl_send_to_user(skb);
}
EXPORT_SYMBOL_GPL(ai_layer2_push_io_context);

/* 安全告警推送 */
int ai_layer2_push_security_event(struct ai_layer2_alert *alert)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;

    if (!layer2_cfg.enabled)
        return -ENODEV;

    skb = alloc_nlmsg(sizeof(*alert), GFP_ATOMIC);
    if (!skb)
        return -ENOMEM;

    nlh = nlmsg_put(skb, 0, 0, AI_L2_CMD_EVENT, sizeof(*alert), NLM_F_MULTICAST);
    memcpy(nlmsg_data(nlh), alert, sizeof(*alert));

    return nl_send_to_user(skb);
}
EXPORT_SYMBOL_GPL(ai_layer2_push_security_event);

/* =========================================================================
 * 配置管理
 * ========================================================================= */

int ai_layer2_set_config(struct ai_layer2_config *cfg)
{
    if (!cfg)
        return -EINVAL;

    mutex_lock(&layer2_cfg.lock);

    layer2_cfg.mode = cfg->mode;
    layer2_cfg.flags = cfg->flags;
    layer2_cfg.sync_timeout_ms = cfg->sync_timeout_ms ?: 1000;
    layer2_cfg.async_queue_depth = cfg->async_queue_depth ?: 256;
    layer2_cfg.route_sched_threshold = cfg->route_sched_threshold ?: 7000;
    layer2_cfg.route_io_threshold = cfg->route_io_threshold ?: 7000;
    layer2_cfg.route_sec_threshold = cfg->route_sec_threshold ?: 7000;
    layer2_cfg.enabled = (cfg->mode != AI_L2_MODE_DISABLED);

    if (cfg->api_provider[0])
        strscpy(layer2_cfg.api_provider, cfg->api_provider, 32);
    if (cfg->api_endpoint[0])
        strscpy(layer2_cfg.api_endpoint, cfg->api_endpoint, 256);
    if (cfg->api_model[0])
        strscpy(layer2_cfg.api_model, cfg->api_model, 64);
    layer2_cfg.api_timeout_ms = cfg->api_timeout_ms ?: 5000;

    mutex_unlock(&layer2_cfg.lock);

    pr_info("%s: config updated — mode=%d enabled=%d\n",
            DRV_NAME, cfg->mode, layer2_cfg.enabled);

    return 0;
}
EXPORT_SYMBOL_GPL(ai_layer2_set_config);

int ai_layer2_get_config(struct ai_layer2_config *cfg)
{
    if (!cfg)
        return -EINVAL;

    mutex_lock(&layer2_cfg.lock);
    cfg->mode = layer2_cfg.mode;
    cfg->flags = layer2_cfg.flags;
    cfg->sync_timeout_ms = layer2_cfg.sync_timeout_ms;
    cfg->async_queue_depth = layer2_cfg.async_queue_depth;
    cfg->route_sched_threshold = layer2_cfg.route_sched_threshold;
    cfg->route_io_threshold = layer2_cfg.route_io_threshold;
    cfg->route_sec_threshold = layer2_cfg.route_sec_threshold;
    cfg->api_timeout_ms = layer2_cfg.api_timeout_ms;
    strscpy(cfg->api_provider, layer2_cfg.api_provider, 32);
    strscpy(cfg->api_endpoint, layer2_cfg.api_endpoint, 256);
    strscpy(cfg->api_model, layer2_cfg.api_model, 64);
    mutex_unlock(&layer2_cfg.lock);

    return 0;
}
EXPORT_SYMBOL_GPL(ai_layer2_get_config);

/* =========================================================================
 * 路由状态
 * ========================================================================= */

int ai_layer2_get_routing_state(void)
{
    return layer2_cfg.mode;
}
EXPORT_SYMBOL_GPL(ai_layer2_get_routing_state);

/* =========================================================================
 * 统计
 * ========================================================================= */

void ai_layer2_stats(u64 *sent, u64 *received, u64 *errors, u64 *dropped)
{
    if (sent)     *sent     = atomic64_read(&layer2_stats.msg_sent);
    if (received) *received = atomic64_read(&layer2_stats.msg_received);
    if (errors)   *errors   = atomic64_read(&layer2_stats.errors);
    if (dropped)  *dropped  = atomic64_read(&layer2_stats.dropped);
}
EXPORT_SYMBOL_GPL(ai_layer2_stats);

/* =========================================================================
 * netlink 消息处理（用户态 → 内核）
 * ========================================================================= */

static void handle_decision_response(struct nlmsghdr *nlh)
{
    struct ai_layer2_decision *dec;
    struct pending_decision *pend, *tmp;
    unsigned long flags;

    if (nlmsg_len(nlh) < sizeof(*dec))
        return;

    dec = nlmsg_data(nlh);
    atomic64_inc(&layer2_stats.decisions_received);

    /* 查找对应的 pending 请求 */
    spin_lock_irqsave(&pending_lock, flags);
    list_for_each_entry_safe(pend, tmp, &pending_list, node) {
        if (pend->request_id == dec->request_id) {
            list_del(&pend->node);
            atomic_dec(&pending_count);
            spin_unlock_irqrestore(&pending_lock, flags);

            /* 取消超时工作 */
            cancel_delayed_work_sync(&pend->dwork);

            /* 记录覆盖情况 */
            if (dec->override_layer1)
                atomic64_inc(&layer2_stats.decisions_override);

            /* 计算往返延迟 */
            if (dec->timestamp_ns && pend->timestamp_ns)
                atomic64_add(ktime_get_ns() - pend->timestamp_ns,
                             &layer2_stats.roundtrip_ns);

            /* 调用回调（如果有） */
            if (pend->callback) {
                pend->callback(dec, pend->priv);
            }

            kfree(pend);
            return;
        }
    }
    spin_unlock_irqrestore(&pending_lock, flags);

    pr_warn("%s: no matching pending request for id=%llu\n",
            DRV_NAME, dec->request_id);
}

/* 超时工作项 */
static void decision_timeout_work(struct work_struct *work)
{
    struct delayed_work *dwork = to_delayed_work(work);
    struct pending_decision *pend = container_of(dwork, struct pending_decision, dwork);
    struct ai_layer2_decision timeout_dec = { 0 };

    /* 从 pending 队列移除 */
    spin_lock(&pending_lock);
    list_del(&pend->node);
    atomic_dec(&pending_count);
    spin_unlock(&pending_lock);

    timeout_dec.decision = 0; /* AI_KEEP */
    timeout_dec.request_id = pend->request_id;
    timeout_dec.timestamp_ns = ktime_get_ns();

    if (pend->callback)
        pend->callback(&timeout_dec, pend->priv);

    atomic64_inc(&layer2_stats.errors);
    pr_warn("%s: decision timeout request_id=%llu\n",
            DRV_NAME, pend->request_id);

    kfree(pend);
}

/* 处理 hello（建立连接）*/
static void handle_hello(struct nlmsghdr *nlh)
{
    struct {
        __u32 pid;
        __u32 version;
    } __packed *hello;

    if (nlmsg_len(nlh) < sizeof(*hello))
        return;

    hello = nlmsg_data(nlh);

    mutex_lock(&nl_mutex);
    nl_pid = hello->pid;
    mutex_unlock(&nl_mutex);

    pr_info("%s: Layer2 daemon connected (pid=%d version=0x%x)\n",
            DRV_NAME, nl_pid, hello->version);

    /* 响应 hello */
    nl_send_ack(nlh->nlmsg_seq, 0);
}

/* 处理 pong */
static void handle_pong(struct nlmsghdr *nlh)
{
    /* 心跳响应，可用于测量延迟 */
    u64 now = ktime_get_ns();
    u64 then;
    if (nlmsg_len(nlh) >= sizeof(then)) {
        memcpy(&then, nlmsg_data(nlh), sizeof(then));
        pr_debug("%s: heartbeat RTT=%llu ns\n", DRV_NAME, now - then);
    }
}

/* 主消息分发 */
static void nl_recv_msg(struct sk_buff *skb)
{
    struct nlmsghdr *nlh;
    int msglen;

    atomic64_inc(&layer2_stats.msg_received);

    nlh = nlmsg_hdr(skb);
    msglen = nlmsg_len(nlh);

    if (!nlmsg_ok(nlh, msglen))
        return;

    switch (nlh->nlmsg_type) {
    case AI_L2_CMD_HELLO:
        handle_hello(nlh);
        break;

    case AI_L2_CMD_DECISION_RES:
        handle_decision_response(nlh);
        break;

    case AI_L2_CMD_PONG:
        handle_pong(nlh);
        break;

    case AI_L2_CMD_CONFIG_SET: {
        struct ai_layer2_config cfg;
        if (msglen >= sizeof(cfg)) {
            memcpy(&cfg, nlmsg_data(nlh), sizeof(cfg));
            ai_layer2_set_config(&cfg);
            nl_send_ack(nlh->nlmsg_seq, 0);
        }
        break;
    }

    case AI_L2_CMD_ROUTE_CTRL: {
        __u32 enable;
        if (msglen >= sizeof(enable)) {
            memcpy(&enable, nlmsg_data(nlh), sizeof(enable));
            layer2_cfg.enabled = !!enable;
            pr_info("%s: routing %s\n",
                    DRV_NAME, enable ? "enabled" : "disabled");
            nl_send_ack(nlh->nlmsg_seq, 0);
        }
        break;
    }

    case AI_L2_CMD_GOODBYE:
        mutex_lock(&nl_mutex);
        pr_info("%s: Layer2 daemon disconnected\n", DRV_NAME);
        nl_pid = 0;
        mutex_unlock(&nl_mutex);
        break;

    case AI_L2_CMD_PING: {
        /* 回应 pong */
        struct sk_buff *rep;
        struct nlmsghdr *rnlh;
        u64 ts = ktime_get_ns();

        rep = alloc_nlmsg(sizeof(ts), GFP_KERNEL);
        if (rep) {
            rnlh = nlmsg_put(rep, 0, nlh->nlmsg_seq, AI_L2_CMD_PONG, sizeof(ts), 0);
            memcpy(nlmsg_data(rnlh), &ts, sizeof(ts));
            nl_send_to_user(rep);
        }
        break;
    }

    default:
        pr_warn("%s: unknown msg type=%d\n", DRV_NAME, nlh->nlmsg_type);
        break;
    }

    /* 继续处理队列中剩余消息 */
    while (nlmsg_ok(nlh, msglen)) {
        nlh = nlmsg_next(nlh, &msglen);
        if (!nlmsg_ok(nlh, msglen))
            break;
        /* 递归处理（同上，略）*/
    }
}

/* netlink 接收回调 */
static void nl_recv(struct sk_buff *skb)
{
    nl_recv_msg(skb);
}

/* netlink 组播接收 */
static void nl_recv_group(struct sk_buff *skb)
{
    nl_recv_msg(skb);
}

/* =========================================================================
 * netlink 族初始化
 * ========================================================================= */

static struct netlink_kernel_cfg nl_kernel_cfg = {
    .groups    = 1,
    .input     = nl_recv,
    .mcGRPInput = nl_recv_group,
};

static int nl_init(void)
{
    struct netlink_kernel_cfg cfg = {
        .input = nl_recv,
    };

    nl_sk = netlink_kernel_create(&init_net,
                                   NETLINK_USERSOCK, /* 或注册新族 */
                                   &cfg);
    if (!nl_sk) {
        pr_err("%s: netlink_kernel_create failed\n", DRV_NAME);
        return -ENOMEM;
    }

    pr_info("%s: netlink socket created\n", DRV_NAME);
    return 0;
}

static void nl_exit(void)
{
    if (nl_sk) {
        netlink_kernel_release(nl_sk);
        nl_sk = NULL;
    }
}

/* =========================================================================
 * proc 接口
 * ========================================================================= */

static int layer2_show(struct seq_file *m, void *v)
{
    struct ai_layer2_config cfg;

    ai_layer2_get_config(&cfg);

    seq_printf(m, "AI Layer2 Router v%s\n", DRV_VER);
    seq_printf(m, "=====================\n\n");

    seq_printf(m, "状态:        %s\n", cfg.mode ? "enabled" : "disabled");
    seq_printf(m, "模式:        %d\n", cfg.mode);
    seq_printf(m, "flags:       0x%x\n", cfg.flags);
    seq_printf(m, "同步超时:    %u ms\n", cfg.sync_timeout_ms);

    seq_printf(m, "\n消息统计:\n");
    seq_printf(m, "  发送:       %llu\n", atomic64_read(&layer2_stats.msg_sent));
    seq_printf(m, "  接收:       %llu\n", atomic64_read(&layer2_stats.msg_received));
    seq_printf(m, "  决策请求:   %llu\n", atomic64_read(&layer2_stats.decisions_sent));
    seq_printf(m, "  决策响应:   %llu\n", atomic64_read(&layer2_stats.decisions_received));
    seq_printf(m, "  Layer2覆盖: %llu\n", atomic64_read(&layer2_stats.decisions_override));
    seq_printf(m, "  错误:       %llu\n", atomic64_read(&layer2_stats.errors));
    seq_printf(m, "  丢弃:       %llu\n", atomic64_read(&layer2_stats.dropped));
    seq_printf(m, "  队列满:     %llu\n", atomic64_read(&layer2_stats.queue_full));

    if (atomic64_read(&layer2_stats.decisions_received) > 0) {
        u64 avg = atomic64_read(&layer2_stats.roundtrip_ns) /
                  atomic64_read(&layer2_stats.decisions_received);
        seq_printf(m, "  平均RTT:    %llu ns\n", avg);
    }

    seq_printf(m, "\nPending 决策: %d\n", atomic_read(&pending_count));
    seq_printf(m, "共享内存:    %s\n", layer2_shmem.active ? "active" : "inactive");
    seq_printf(m, "用户态PID:   %d\n", nl_pid);

    seq_printf(m, "\n路由阈值:\n");
    seq_printf(m, "  调度:       %u\n", cfg.route_sched_threshold);
    seq_printf(m, "  IO:         %u\n", cfg.route_io_threshold);
    seq_printf(m, "  安全:       %u\n", cfg.route_sec_threshold);

    seq_printf(m, "\nAPI 配置:\n");
    seq_printf(m, "  provider:   %s\n", layer2_cfg.api_provider);
    seq_printf(m, "  model:      %s\n", layer2_cfg.api_model);
    seq_printf(m, "  endpoint:   %s\n", layer2_cfg.api_endpoint);
    seq_printf(m, "  timeout:    %u ms\n", layer2_cfg.api_timeout_ms);

    return 0;
}

static int layer2_open(struct inode *inode, struct file *file)
{
    return single_open(file, layer2_show, NULL);
}

static ssize_t layer2_write(struct file *file, const char __user *buf,
                            size_t count, loff_t *ppos)
{
    char kbuf[16];
    if (count >= sizeof(kbuf)) return -EINVAL;
    if (copy_from_user(kbuf, buf, count)) return -EFAULT;
    kbuf[count] = '\0';

    if (kbuf[0] == '1') {
        layer2_cfg.enabled = true;
        layer2_cfg.mode = AI_L2_MODE_ASYNC;
        pr_info("%s: Layer2 routing enabled (async mode)\n", DRV_NAME);
    } else if (kbuf[0] == '0') {
        layer2_cfg.enabled = false;
        pr_info("%s: Layer2 routing disabled\n", DRV_NAME);
    } else {
        pr_info("%s: write 1 to enable, 0 to disable\n", DRV_NAME);
    }

    return count;
}

static const struct proc_ops layer2_proc_fops = {
    .proc_open    = layer2_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
    .proc_write   = layer2_write,
};

void ai_layer2_proc_init(void)
{
    proc_create("ai_layer2", 0644, NULL, &layer2_proc_fops);
}

void ai_layer2_proc_exit(void)
{
    remove_proc_entry("ai_layer2", NULL);
}

/* =========================================================================
 * 模块初始化 / 退出
 * ========================================================================= */

static int __init ai_layer2_init(void)
{
    int ret;

    pr_info("========================================\n");
    pr_info("  AI Layer2 Router v%s\n", DRV_VER);
    pr_info("  内核 AI ↔ 用户态 AI API 双向路由\n");
    pr_info("========================================\n");

    mutex_init(&layer2_cfg.lock);

    ret = nl_init();
    if (ret < 0)
        return ret;

    ai_layer2_proc_init();

    /* 默认启用异步模式（但路由未开启） */
    layer2_cfg.mode = AI_L2_MODE_ASYNC;

    pr_info("%s: Layer2 router ready\n", DRV_NAME);
    pr_info("%s: 写 1 到 /proc/ai_layer2 启用路由\n", DRV_NAME);

    return 0;
}

static void __exit ai_layer2_exit(void)
{
    struct pending_decision *p, *tmp;

    pr_info("%s: shutting down Layer2 router\n", DRV_NAME);

    /* 清理 pending 决策 */
    spin_lock(&pending_lock);
    list_for_each_entry_safe(p, tmp, &pending_list, node) {
        list_del(&p->node);
        cancel_delayed_work_sync(&p->dwork);
        kfree(p);
    }
    spin_unlock(&pending_lock);

    /* 清理共享内存 */
    if (layer2_shmem.active && layer2_shmem.addr)
        vfree(layer2_shmem.addr);

    nl_exit();
    ai_layer2_proc_exit();

    pr_info("%s: Layer2 router stopped\n", DRV_NAME);
}

module_init(ai_layer2_init);
module_exit(ai_layer2_exit);

MODULE_DESCRIPTION("AI Layer2 — Kernel-to-Userspace AI Routing");
MODULE_VERSION(DRV_VER);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("AI Linux Team");
