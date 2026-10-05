/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * nr3ehid.kpm — syscall(__NR_socket=198)+magic 邮箱
 *
 * aarch64 上 socket=198（dbc-rw 用的 41 是 pivot_root，游戏/seccomp 不友好）。
 * 真实 socket() 的 x0 是 AF_*（很小），不会撞上 magic。
 */
#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <common.h>
#include <kputils.h>
#include <hook.h>
#include <syscall.h>
#include <asm/current.h>
#include <asm/ptrace.h>
#include <uapi/asm-generic/unistd.h>
#include "../common/kpm_demo_helpers.h"

#ifndef STEALTH_FUSED
KPM_MODULE_INFO("nr3ehid",
                "1.3.1",
                "GPL v2",
                "nr3e",
                "socket mailbox IPC (no getdents)");
#endif

#ifndef __NR_socket
#define __NR_socket 198
#endif

#define X_MAGIC      0x4E33C0DE5A1E0000ULL
#define X_MASK       0xFFFFFFFFFFFFFF00ULL
#define CMD_PING     0
#define CMD_BIND     1
#define CMD_POST     2
#define CMD_TAKE     3
#define CMD_REPLY    4
#define CMD_POLL     5
#define CMD_HIDETID  6
#define CMD_HIDEPID  7
#define PING_RET     0x4E33

#define MAX_HIDE_TID 32
#define EAGAIN_R     (-11)
#define EBUSY_R      (-16)
#define EINVAL_R     (-22)
#define EFAULT_R     (-14)

enum pid_type {
    PIDTYPE_PID,
    PIDTYPE_TGID,
    PIDTYPE_PGID,
    PIDTYPE_SID,
    PIDTYPE_MAX,
};
struct pid_namespace;
struct task_struct;

static pid_t (*k_task_pid_nr_ns)(struct task_struct *task, enum pid_type type,
                                 struct pid_namespace *ns);

struct nr3e_take {
    int32_t cmd;
    int32_t extra;
};

struct nr3e_kreply {
    int32_t enabled;
    int32_t touched;
    char error[192];
    char message[192];
};

static volatile int g_pending;
static pid_t g_tgid;
static int32_t g_cmd;
static int32_t g_extra;
static struct nr3e_kreply g_reply;
static pid_t g_hide_tid[MAX_HIDE_TID];
static int g_nr_hide;

static int g_hooked_sys;

static void nzero(void *dst, unsigned long n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = 0;
}

static pid_t cur_pid(enum pid_type t)
{
    if (!k_task_pid_nr_ns)
        return 0;
    return k_task_pid_nr_ns(current, t, 0);
}

static int tid_hidden(pid_t tid)
{
    int i;
    if (tid <= 0)
        return 0;
    for (i = 0; i < g_nr_hide; i++) {
        if (g_hide_tid[i] == tid)
            return 1;
    }
    return 0;
}

static int add_hide_tid(pid_t tid)
{
    if (tid <= 0)
        return EINVAL_R;
    if (tid_hidden(tid))
        return 0;
    if (g_nr_hide >= MAX_HIDE_TID)
        return -28;
    g_hide_tid[g_nr_hide++] = tid;
    return 0;
}

static int parse_tid(const char *s, int n, pid_t *out)
{
    int i;
    unsigned v = 0;
    if (!s || n <= 0 || n > 10)
        return 0;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10u + (unsigned)(s[i] - '0');
    }
    if (!v)
        return 0;
    *out = (pid_t)v;
    return 1;
}

static int copy_out(unsigned long uptr, const void *src, int n)
{
    if (!uptr || !src || n <= 0)
        return -1;
    if (compat_copy_to_user((void __user *)uptr, src, n))
        return -1;
    return 0;
}

static void set_ret(hook_fargs6_t *args, long ret)
{
    args->ret = (uint64_t)ret;
    args->skip_origin = 1;
    args->local.data0 = (uint64_t)ret;
    args->local.data1 = 1;
    if (has_syscall_wrapper) {
        struct pt_regs *regs = (struct pt_regs *)args->args[0];
        if (regs)
            regs->regs[0] = (uint64_t)ret;
    }
}

static void xcall_after(hook_fargs6_t *args, void *udata)
{
    (void)udata;
    if (!args->local.data1)
        return;
    set_ret(args, (long)args->local.data0);
}

static void xcall(hook_fargs6_t *args, void *udata)
{
    uint64_t *raw;
    uint64_t magic;
    int cmd;
    pid_t me, tgid;
    struct nr3e_take take;
    long n;

    (void)udata;
    args->local.data1 = 0;
    raw = syscall_args(args);
    magic = raw[0];
    if ((magic & X_MASK) != X_MAGIC)
        return;

    cmd = (int)(magic & 0xFF);
    me = cur_pid(PIDTYPE_PID);
    tgid = cur_pid(PIDTYPE_TGID);

    switch (cmd) {
    case CMD_PING:
        set_ret(args, PING_RET);
        break;

    case CMD_BIND:
        g_tgid = tgid;
        g_pending = 0;
        set_ret(args, (long)g_tgid);
        break;

    case CMD_POST:
        if (!g_tgid) {
            set_ret(args, EINVAL_R);
            break;
        }
        /* 1=queued 3=running：不能覆盖。2=未取回复：丢掉旧回复再发。 */
        if (g_pending == 1 || g_pending == 3) {
            set_ret(args, EBUSY_R);
            break;
        }
        g_cmd = (int32_t)raw[1];
        g_extra = (int32_t)raw[2];
        nzero(&g_reply, sizeof(g_reply));
        g_pending = 1;
        set_ret(args, 0);
        break;

    case CMD_TAKE:
        if (!g_tgid || tgid != g_tgid) {
            set_ret(args, EINVAL_R);
            break;
        }
        if (g_pending != 1) {
            set_ret(args, EAGAIN_R);
            break;
        }
        take.cmd = g_cmd;
        take.extra = g_extra;
        if (copy_out(raw[1], &take, (int)sizeof(take))) {
            set_ret(args, EFAULT_R);
            break;
        }
        g_pending = 3;
        set_ret(args, 0);
        break;

    case CMD_REPLY:
        if (!g_tgid || tgid != g_tgid) {
            set_ret(args, EINVAL_R);
            break;
        }
        nzero(&g_reply, sizeof(g_reply));
        g_reply.enabled = (int32_t)raw[1];
        g_reply.touched = (int32_t)raw[2];
        if (raw[3]) {
            n = compat_strncpy_from_user(g_reply.error,
                                         (const char __user *)raw[3],
                                         sizeof(g_reply.error) - 1);
            if (n < 0)
                g_reply.error[0] = 0;
            g_reply.error[sizeof(g_reply.error) - 1] = 0;
        }
        if (raw[4]) {
            n = compat_strncpy_from_user(g_reply.message,
                                         (const char __user *)raw[4],
                                         sizeof(g_reply.message) - 1);
            if (n < 0)
                g_reply.message[0] = 0;
            g_reply.message[sizeof(g_reply.message) - 1] = 0;
        }
        g_pending = 2;
        set_ret(args, 0);
        break;

    case CMD_POLL:
        if (!raw[1]) {
            set_ret(args, EINVAL_R);
            break;
        }
        if (g_pending == 2) {
            if (copy_out(raw[1], &g_reply, (int)sizeof(g_reply))) {
                set_ret(args, EFAULT_R);
                break;
            }
            g_pending = 0;
            set_ret(args, 2);
        } else if (g_pending == 1 || g_pending == 3) {
            set_ret(args, 1);
        } else {
            set_ret(args, 0);
        }
        break;

    case CMD_HIDETID: {
        pid_t tid = (pid_t)raw[1];
        if (!tid)
            tid = me;
        set_ret(args, add_hide_tid(tid));
        break;
    }

    case CMD_HIDEPID: {
        pid_t pid = (pid_t)raw[1];
        if (!pid)
            pid = tgid;
        set_ret(args, add_hide_tid(pid));
        break;
    }

    default:
        set_ret(args, EINVAL_R);
        break;
    }
}

static int install_hooks(void)
{
    int rc;

    k_task_pid_nr_ns = (void *)kallsyms_lookup_name("__task_pid_nr_ns");
    if (!k_task_pid_nr_ns)
        k_task_pid_nr_ns = (void *)kallsyms_lookup_name("task_pid_nr_ns");

    rc = fp_hook_syscalln(__NR_socket, 6, xcall, xcall_after, 0);
    if (rc != 0) {
        pr_err("nr3ehid: fp_hook socket fail rc=%d\n", rc);
        return -1;
    }
    g_hooked_sys = 1;
    return 0;
}

static void remove_hooks(void)
{
    if (g_hooked_sys) {
        fp_unhook_syscalln(__NR_socket, xcall, xcall_after);
        g_hooked_sys = 0;
    }
}

long nr3ehid_init(const char *args, const char *event, void *__user reserved)
{
    (void)args;
    (void)event;
    (void)reserved;
    g_hooked_sys = 0;
    g_pending = 0;
    g_tgid = 0;
    g_nr_hide = 0;
    g_cmd = 0;
    g_extra = 0;
    nzero(g_hide_tid, sizeof(g_hide_tid));
    nzero(&g_reply, sizeof(g_reply));
    if (install_hooks() < 0) {
        remove_hooks();
        return -1;
    }
    return 0;
}

long nr3ehid_exit(void *__user reserved)
{
    (void)reserved;
    remove_hooks();
    g_nr_hide = 0;
    g_tgid = 0;
    g_pending = 0;
    return 0;
}

long nr3ehid_ctl0(const char *args, char *__user out_msg, int outlen)
{
    static const char msg[] = "nr3ehid socket198";
    (void)args;
    return kpm_demo_copy_message(msg, out_msg, outlen);
}

#ifndef STEALTH_FUSED
KPM_INIT(nr3ehid_init);
KPM_CTL0(nr3ehid_ctl0);
KPM_EXIT(nr3ehid_exit);
#endif
