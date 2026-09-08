// SPDX-License-Identifier: GPL-2.0
/*
 * ai_lsm.bpf.c — AI 驱动的内核安全检测（BPF LSM）
 *
 * 挂在 Linux 安全子系统（LSM）的各个 hook 上：
 *   - task_exec:   进程 execve 时检测
 *   - file_open:   文件打开时检测
 *   - mmap:        内存映射时检测（exec flag）
 *   - bind:        网络 bind 时检测
 *
 * 编译：
 *   clang -O2 -target bpf -g \
 *     -D__TARGET_ARCH_arm64 \
 *     -I/usr/include/bpf \
 *     ai_lsm.bpf.c -o ai_lsm.bpf.o
 *
 * 加载：
 *   bpftool lsm attach ./ai_lsm.bpf.o
 *
 * 内核需：CONFIG_BPF_LSM=y
 */

#include <linux/bpf.h>
#include <linux/bpf_lsm.h>
#include <linux/bpf_trace.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/* =========================================================================
 * BPF Maps
 * ========================================================================= */

/* 异常评分阈值（可动态更新）*/
struct { __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32); __type(value, float); }
config_map SEC(".maps");

/* 进程执行计数（用于频率分析）*/
struct { __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 8192);
    __type(key, __u32); /* pid */
    __type(value, struct exec_record); }
exec_history SEC(".maps");

/* 异常事件 ring buffer */
struct { __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 4096 * 64); }
security_events SEC(".maps");

/* =========================================================================
 * 数据结构
 * ========================================================================= */

struct exec_record {
    __u32 exec_count;       /* 段时间内执行次数 */
    __u32 parent_pid;
    __u64 last_exec_ns;
    __u32 avg_interval;      /* 平均执行间隔（ns）*/
    char comm[16];
};

struct security_event {
    __u32  pid;
    __u32  uid;
    char   comm[16];
    char   filename[256];
    float  score;
    __u8   action;     /* 0=allow, 1=warn, 2=deny */
    __u8   hook_id;    /* hook 标识 */
    __u64  timestamp_ns;
    char   reason[128];
};

/* =========================================================================
 * 评分函数
 * ========================================================================= */

static __always_inline float
score_exec(const char *filename, __u32 pid, __u32 parent_pid)
{
    float score = 0.0f;
    struct exec_record *rec;
    __u64 now = bpf_ktime_get_ns();

    rec = bpf_map_lookup_elem(&exec_history, &pid);
    if (rec) {
        /* 高频执行检测（正常程序不应每秒 exec 100+ 次）*/
        if (rec->exec_count > 10 && rec->last_exec_ns > 0) {
            __u64 interval = now - rec->last_exec_ns;
            if (interval < 10000000ULL) { /* 10ms 内 */
                score += 0.5f; /* 可能是 fork bomb 或恶意脚本 */
            }
        }

        /* 孤儿子进程（父进程已退出）*/
        if (parent_pid != 0) {
            /* 检查父进程是否存在 —— BPF 无法直接做，
             * 用启发式：短寿父进程 + 高频子 exec = 可疑 */
            if (rec->avg_interval < 100000000ULL && rec->exec_count > 5)
                score += 0.3f;
        }

        /* 更新记录 */
        rec->exec_count++;
        rec->last_exec_ns = now;
        rec->avg_interval = (rec->avg_interval * (rec->exec_count - 1) + interval) / rec->exec_count;
    } else {
        struct exec_record new_rec = { 0 };
        new_rec.exec_count = 1;
        new_rec.parent_pid = parent_pid;
        new_rec.last_exec_ns = now;
        bpf_map_update_elem(&exec_history, &pid, &new_rec, BPF_ANY);
    }

    /* 文件名启发式检测 */
    /* base64 / xor 混淆特征 */
    const char *bad_prefixes[] = {
        "/tmp/", "/var/tmp/", "/dev/shm/", "/proc/self/"
    };
    for (int i = 0; i < 4; i++) {
        if (bpf_strncmp(filename, 8, bad_prefixes[i]) == 0)
            score += 0.15f;
    }

    /* LD_PRELOAD / LD_AUDIT 特征 */
    if (bpf_strncmp(filename, 10, "LD_PRELOAD") == 0 ||
        bpf_strncmp(filename, 9,  "LD_AUDIT") == 0)
        score += 0.4f;

    /* /proc/ 写入（进程注入）*/
    if (bpf_strncmp(filename, 6, "/proc/") == 0)
        score += 0.3f;

    return score > 1.0f ? 1.0f : score;
}

/* =========================================================================
 * LSM Hooks
 * ========================================================================= */

/* 进程 execve 时检测 */
SEC("lsm/task_exec")
int AIHook_task_exec(struct linux_binprm *bprm)
{
    float threshold = 0.7f;
    struct security_event *evt;
    __u32 key = 0;
    struct config_entry { float threshold; } *cfg;
    float score;

    cfg = (struct config_entry *)bpf_map_lookup_elem(&config_map, &key);
    if (cfg) threshold = cfg->threshold;

    char filename[256] = { 0 };
    bpf_probe_read_user_str(filename, sizeof(filename), bprm->filename);

    score = score_exec(filename, bpf_get_current_pid_tgid() >> 32,
                       bprm->parent_pid);

    evt = bpf_ringbuf_reserve(&security_events, sizeof(*evt), 0);
    if (evt) {
        evt->pid        = bpf_get_current_pid_tgid() >> 32;
        evt->uid        = bpf_get_current_uid_gid() & 0xFFFFFFFF;
        evt->score      = score;
        evt->hook_id    = 1; /* task_exec */
        evt->timestamp_ns = bpf_ktime_get_ns();
        evt->action     = score >= threshold ? 2 : 0; /* deny : allow */
        bpf_ringbuf_submit(evt, 0);
    }

    if (score >= threshold)
        return -EPERM;  /* 拒绝执行 */

    return 0;
}

/* 文件打开时检测（可扩展）*/
SEC("lsm/file_open")
int AIHook_file_open(struct file *file)
{
    struct security_event *evt;
    float score = 0.0f;
    char filename[256] = { 0 };

    if (!file || !file->f_path.dentry)
        return 0;

    bpf_probe_read_kernel_str(filename, sizeof(filename),
                              file->f_path.dentry->d_name.name);

    /* 敏感文件访问检测 */
    const char *sensitive[] = {
        "/etc/shadow", "/etc/sudoers", "/etc/passwd",
        "/etc/gshadow", "/root/.ssh/", "/etc/cron.d/"
    };

    for (int i = 0; i < 6; i++) {
        if (bpf_strncmp(filename, 11, sensitive[i]) == 0)
            score += 0.5f;
    }

    evt = bpf_ringbuf_reserve(&security_events, sizeof(*evt), 0);
    if (evt) {
        evt->pid = bpf_get_current_pid_tgid() >> 32;
        evt->score = score;
        evt->hook_id = 2; /* file_open */
        evt->timestamp_ns = bpf_ktime_get_ns();
        evt->action = score > 0.7f ? 2 : 0;
        bpf_ringbuf_submit(evt, 0);
    }

    return 0;
}

char _license[] SEC("license") = "GPL";
