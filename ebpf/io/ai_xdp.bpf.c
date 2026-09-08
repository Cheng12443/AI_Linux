// SPDX-License-Identifier: GPL-2.0
/*
 * ai_xdp.bpf.c — AI 驱动的 XDP 网络异常检测
 *
 * 在网卡驱动层（XDP）做流量分类：
 *   - 提取 packet 特征（IP, port, packet_len, flags...）
 *   - 调用 AI 模型评分（0~1）
 *   - >0.9 → DROP（拒绝恶意流量）
 *   - >0.7 → REDIRECT 到慢路径处理
 *   - <0.7 → 正常转发
 *
 * 编译：
 *   clang -O2 -target bpf -g \
 *     -I/usr/include/bpf \
 *     ai_xdp.bpf.c -o ai_xdp.bpf.o
 *
 * 加载：
 *   ip link set dev eth0 xdp obj ai_xdp.bpf.o sec xdp
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

struct ai_xdp_config {
    float drop_threshold;
    float slow_path_threshold;
    __u32  sample_rate;
};

struct traffic_baseline {
    __u32 packet_count;
    __u32 byte_count;
    __u32 last_seen_ns;
};

struct packet_features {
    __u32 src_ip;
    __u32 dst_ip;
    __u16 src_port;
    __u16 dst_port;
    __u8  protocol;
    __u8  tcp_flags;
    __u32 packet_len;
    __u8  ip_ttl;
    __u8  ip_tos;
};

struct ai_xdp_event {
    __u32  src_ip;
    __u32  dst_ip;
    __u16  src_port;
    __u16  dst_port;
    __u8   protocol;
    __u8   action;
    float  score;
    __u32  packet_len;
    __u64  timestamp_ns;
};

struct { __uint(type, BPF_MAP_TYPE_HASH); __uint(max_entries, 65536);
    __type(key, __u32); __type(value, struct traffic_baseline); }
traffic_baseline SEC(".maps");

struct { __uint(type, BPF_MAP_TYPE_HASH); __uint(max_entries, 32768);
    __type(key, __u64); __type(value, float); }
packet_scores SEC(".maps");

struct { __uint(type, BPF_MAP_TYPE_ARRAY); __uint(max_entries, 1);
    __type(key, __u32); __type(value, struct ai_xdp_config); }
ai_config SEC(".maps");

struct { __uint(type, BPF_MAP_TYPE_RINGBUF); __uint(max_entries, 4096 * 64); }
ai_io_events SEC(".maps");

static __always_inline float score_packet(const struct packet_features *pf)
{
    float score = 0.0f;

    /* SSH 暴力破解特征：仅 SYN 标志，短包 */
    if (pf->dst_port == 22 && pf->tcp_flags == 2 && pf->packet_len < 64)
        score += 0.4f;

    /* DNS 放大：UDP dst_port=53，小包 */
    if (pf->dst_port == 53 && pf->protocol == 17 && pf->packet_len < 128)
        score += 0.3f;

    /* NULL 扫描：无 TCP 标志 */
    if (pf->protocol == 6 && pf->tcp_flags == 0)
        score += 0.5f;

    /* Xmas 扫描：FIN+PSH+URG = 0x29 */
    if (pf->protocol == 6 && pf->tcp_flags == 0x29)
        score += 0.5f;

    /* TTL 异常：可能伪造源 */
    if (pf->ip_ttl < 32)
        score += 0.2f;

    return score > 1.0f ? 1.0f : score;
}

SEC("xdp")
int ai_xdp_classify(struct xdp_md *ctx)
{
    void *data     = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;
    struct ethhdr  *eth = data;
    struct iphdr   *iph;
    struct tcphdr  *tcph;
    struct udphdr  *udph;
    struct packet_features pf = { 0 };
    float score = 0.0f;
    struct ai_xdp_event *evt;
    int action = XDP_PASS;
    struct ai_xdp_config *cfg;
    __u32 key = 0;

    if ((void *)(eth + 1) > data_end) return XDP_PASS;
    if (eth->h_proto != __builtin_bswap16(ETH_P_IP)) return XDP_PASS;

    iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end) return XDP_PASS;

    pf.src_ip     = iph->saddr;
    pf.dst_ip     = iph->daddr;
    pf.protocol   = iph->protocol;
    pf.packet_len = __builtin_bswap16(iph->tot_len);
    pf.ip_ttl     = iph->ttl;
    pf.ip_tos     = iph->tos;

    if (iph->protocol == IPPROTO_TCP) {
        tcph = (void *)iph + iph->ihl * 4;
        if ((void *)(tcph + 1) > data_end) return XDP_PASS;
        pf.src_port  = __builtin_bswap16(tcph->source);
        pf.dst_port  = __builtin_bswap16(tcph->dest);
        pf.tcp_flags = *(const __u8 *)tcph & 0x3F;
    } else if (iph->protocol == IPPROTO_UDP) {
        udph = (void *)iph + iph->ihl * 4;
        if ((void *)(udph + 1) > data_end) return XDP_PASS;
        pf.src_port  = __builtin_bswap16(udph->source);
        pf.dst_port  = __builtin_bswap16(udph->dest);
    }

    /* 更新流量基线 */
    struct traffic_baseline *bl = bpf_map_lookup_elem(&traffic_baseline, &pf.src_ip);
    if (!bl) {
        struct traffic_baseline nb = { .packet_count = 1, .last_seen_ns = bpf_ktime_get_ns() };
        bpf_map_update_elem(&traffic_baseline, &pf.src_ip, &nb, BPF_ANY);
    } else {
        bl->packet_count++;
        bl->last_seen_ns = bpf_ktime_get_ns();
    }

    score = score_packet(&pf);

    cfg = bpf_map_lookup_elem(&ai_config, &key);
    float drop_thresh = cfg ? cfg->drop_threshold : 0.9f;
    float slow_thresh = cfg ? cfg->slow_path_threshold : 0.7f;

    if (score >= drop_thresh)
        action = XDP_DROP;
    else if (score >= slow_thresh)
        action = XDP_REDIRECT;

    evt = bpf_ringbuf_reserve(&ai_io_events, sizeof(*evt), 0);
    if (evt) {
        evt->src_ip = pf.src_ip; evt->dst_ip = pf.dst_ip;
        evt->src_port = pf.src_port; evt->dst_port = pf.dst_port;
        evt->protocol = pf.protocol; evt->action = action;
        evt->score = score; evt->packet_len = pf.packet_len;
        evt->timestamp_ns = bpf_ktime_get_ns();
        bpf_ringbuf_submit(evt, 0);
    }

    return action;
}

char _license[] SEC("license") = "GPL";
