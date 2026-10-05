/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ktouch.kpm — 内核读触摸，给悬浮窗用。
 *
 * hook input_event，不吞事件，游戏照常收到。
 * 只记录多点协议和 BTN_TOUCH。加速度计的 ABS_X/Y 不会登记成触摸设备。
 *
 * 用户态（仅 uid 0，符号缺失时退回 magic）：
 *   prctl(0x4B540001)                         → 0x4B54
 *   prctl(0x4B540010, &struct ktouch_snap)    → 0
 *   prctl(0x4B540020, phase, x, y)            → 0
 *   phase 1 把第 8 槽拖到原始坐标，phase 0 抬手。
 *
 * struct ktouch_snap {
 *   uint32_t magic;   // 0x4B540001
 *   uint32_t seq;
 *   int32_t  down;
 *   int32_t  x, y;    // 触摸屏原始坐标
 *   int32_t  max_x, max_y;
 *   int32_t  slot;
 * };
 */
#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <common.h>
#include <kputils.h>
#include <hook.h>
#include <syscall.h>
#include <asm/ptrace.h>
#include <uapi/asm-generic/unistd.h>
#include "../common/kpm_demo_helpers.h"

#ifndef STEALTH_FUSED
KPM_MODULE_INFO("ktouch",
                "1.1.0",
                "GPL v2",
                "dbc",
                "Kernel touch sample and aim drag (input_event)");
#endif

#define PR_KTOUCH_PING  0x4B540001
#define PR_KTOUCH_GET   0x4B540010
#define PR_KTOUCH_MOVE  0x4B540020
#define KTOUCH_MAGIC    0x4B540001u
#define KTOUCH_PING_RET 0x4B54

#define EV_SYN 0x00u
#define EV_KEY 0x01u
#define EV_ABS 0x03u
#define SYN_REPORT 0
#define BTN_TOUCH 0x14a
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_MT_SLOT 0x2f
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39

#define NDEV 4
#define NSLOT 10
#define EPERM_R (-1)
#define EFAULT_R (-14)
#define EINVAL_R (-22)

#ifndef __NR_prctl
#define __NR_prctl 167
#endif

struct ktouch_snap {
    uint32_t magic;
    uint32_t seq;
    int32_t down;
    int32_t x;
    int32_t y;
    int32_t max_x;
    int32_t max_y;
    int32_t slot;
};

struct abs_info {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

struct finger {
    int32_t x;
    int32_t y;
    int32_t id;
};

struct devst {
    void *dev;
    int touch;
    int slot;
    int btn;
    int32_t sx;
    int32_t sy;
    int32_t max_x;
    int32_t max_y;
    int abs_tried;
    struct finger f[NSLOT];
};

static struct devst g_dev[NDEV];
static struct ktouch_snap g_pub;
static int32_t g_max_x;
static int32_t g_max_y;
static uint32_t g_lockseq;
static uint32_t g_evseq;
static int g_lock;
static int g_hooked_input;
static int g_hooked_prctl;
static void *g_input_fn;
static volatile int g_injecting;
static int g_aim_down;
static long (*k_nofault)(void *dst, const void *src, unsigned long size);
static uint64_t (*k_uidgid)(void);

static void mb_st(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

static void mb_ld(void)
{
    __asm__ volatile("dmb ishld" ::: "memory");
}

/* input_event can run in IRQ context. A spinning acquire on the same CPU
 * deadlocks if the holder is the interrupted thread. Drop the event. */
static int spin_trylock(void)
{
    return !__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE);
}

static void spin_unlock(void)
{
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

static int caller_ok(void)
{
    uint64_t v;

    if (!k_uidgid)
        return 1;
    v = k_uidgid();
    return (uint32_t)v == 0;
}

static int is_kptr(unsigned long p)
{
    return p > 0xffff000000000000UL && (p & 7UL) == 0;
}

static int abs_sane(const struct abs_info *a, int lo, int hi)
{
    if (a->maximum < lo || a->maximum > hi)
        return 0;
    if (a->minimum < -32 || a->minimum > 64)
        return 0;
    if (a->minimum >= a->maximum)
        return 0;
    return 1;
}

/* input_dev->absinfo 偏移随内核变。用 nofault 在对象里找那张表。
 * 槽位轴对不上也收下 X/Y 量程。另一路设备扫失败时不能把已经看到的量程丢掉。 */
static unsigned long g_abs_words[0x1000 / sizeof(unsigned long)];

static void learn_abs(struct devst *d)
{
    int i;

    d->abs_tried = 1;
    if (!k_nofault || !d->dev)
        return;
    if (k_nofault(g_abs_words, d->dev, sizeof(g_abs_words)))
        return;
    for (i = 0; i < (int)(sizeof(g_abs_words) / sizeof(g_abs_words[0])); i++) {
        struct abs_info ax, ay, sl;
        unsigned long p = g_abs_words[i];
        int slot_ok = 0;

        if (!is_kptr(p))
            continue;
        if (k_nofault(&ax, (const void *)(p + ABS_MT_POSITION_X * sizeof(ax)), sizeof(ax)))
            continue;
        if (k_nofault(&ay, (const void *)(p + ABS_MT_POSITION_Y * sizeof(ay)), sizeof(ay)))
            continue;
        if (!abs_sane(&ax, 1000, 65535) || !abs_sane(&ay, 1000, 65535))
            continue;
        if (!k_nofault(&sl, (const void *)(p + ABS_MT_SLOT * sizeof(sl)), sizeof(sl)) &&
            sl.maximum >= 1 && sl.maximum <= 32 && sl.minimum >= -1 && sl.minimum < sl.maximum)
            slot_ok = 1;
        d->max_x = ax.maximum;
        d->max_y = ay.maximum;
        if (ax.maximum > g_max_x)
            g_max_x = ax.maximum;
        if (ay.maximum > g_max_y)
            g_max_y = ay.maximum;
        pr_info("ktouch: abs max %d x %d slot=%d\n", d->max_x, d->max_y, slot_ok);
        return;
    }
    if (g_max_x > 1 && g_max_y > 1) {
        d->max_x = g_max_x;
        d->max_y = g_max_y;
        pr_info("ktouch: abs reused %d x %d\n", d->max_x, d->max_y);
        return;
    }
    pr_info("ktouch: absinfo not found\n");
}

static struct devst *find_dev(void *dev, int create)
{
    int i, s;

    for (i = 0; i < NDEV; i++) {
        if (g_dev[i].dev == dev)
            return &g_dev[i];
    }
    if (!create)
        return NULL;
    for (i = 0; i < NDEV; i++) {
        if (g_dev[i].dev)
            continue;
        g_dev[i].dev = dev;
        g_dev[i].touch = 1;
        g_dev[i].slot = 0;
        for (s = 0; s < NSLOT; s++)
            g_dev[i].f[s].id = -1;
        return &g_dev[i];
    }
    return NULL;
}

static void publish(struct devst *d)
{
    int i, s, any = 0;
    int32_t x = 0, y = 0, mx = 0, my = 0, slot = 0;

    (void)d;
    for (i = 0; i < NDEV; i++) {
        if (g_dev[i].max_x > mx)
            mx = g_dev[i].max_x;
        if (g_dev[i].max_y > my)
            my = g_dev[i].max_y;
    }
    for (i = 0; i < NDEV && !any; i++) {
        for (s = 0; s < NSLOT; s++) {
            if (g_dev[i].f[s].id < 0)
                continue;
            x = g_dev[i].f[s].x;
            y = g_dev[i].f[s].y;
            slot = s;
            any = 1;
            break;
        }
        if (!any && g_dev[i].btn) {
            x = g_dev[i].sx;
            y = g_dev[i].sy;
            slot = g_dev[i].slot;
            any = 1;
        }
    }
    if (mx <= 1)
        mx = g_max_x;
    if (my <= 1)
        my = g_max_y;

    g_lockseq++;
    mb_st();
    g_evseq++;
    g_pub.magic = KTOUCH_MAGIC;
    g_pub.seq = g_evseq;
    g_pub.down = any;
    g_pub.x = x;
    g_pub.y = y;
    g_pub.max_x = mx;
    g_pub.max_y = my;
    g_pub.slot = slot;
    mb_st();
    g_lockseq++;
}

static void note_event(void *dev, unsigned int type, unsigned int code, int value)
{
    struct devst *d;

    if (g_injecting)
        return;
    int slot;
    int mt = type == EV_ABS &&
             (code == ABS_MT_SLOT || code == ABS_MT_TRACKING_ID ||
              code == ABS_MT_POSITION_X || code == ABS_MT_POSITION_Y);
    int btn = type == EV_KEY && code == BTN_TOUCH;

    if (!mt && !btn && type != EV_SYN &&
        !(type == EV_ABS && (code == ABS_X || code == ABS_Y)))
        return;

    if (!spin_trylock())
        return;
    if (mt || btn)
        d = find_dev(dev, 1);
    else
        d = find_dev(dev, 0);
    if (!d || !d->touch) {
        spin_unlock();
        return;
    }

    if ((mt || btn) && !d->abs_tried)
        learn_abs(d);

    if (type == EV_ABS && code == ABS_MT_SLOT) {
        if (value < 0)
            value = 0;
        if (value >= NSLOT)
            value = NSLOT - 1;
        d->slot = value;
    } else if (type == EV_ABS && code == ABS_MT_TRACKING_ID) {
        slot = d->slot;
        if (slot < 0 || slot >= NSLOT)
            slot = 0;
        d->f[slot].id = value;
    } else if (type == EV_ABS && code == ABS_MT_POSITION_X) {
        slot = d->slot;
        if (slot < 0 || slot >= NSLOT)
            slot = 0;
        d->f[slot].x = value;
    } else if (type == EV_ABS && code == ABS_MT_POSITION_Y) {
        slot = d->slot;
        if (slot < 0 || slot >= NSLOT)
            slot = 0;
        d->f[slot].y = value;
    } else if (type == EV_ABS && code == ABS_X) {
        d->sx = value;
    } else if (type == EV_ABS && code == ABS_Y) {
        d->sy = value;
    } else if (btn) {
        d->btn = value ? 1 : 0;
    } else if (type == EV_SYN && code == SYN_REPORT) {
        publish(d);
    }
    spin_unlock();
}

static void input_before(hook_fargs4_t *args, void *udata)
{
    (void)udata;
    if (!args)
        return;
    note_event((void *)args->arg0, (unsigned int)args->arg1,
               (unsigned int)args->arg2, (int)args->arg3);
}

static int read_pub(struct ktouch_snap *out)
{
    int i;

    for (i = 0; i < 16; i++) {
        uint32_t s1 = g_lockseq;

        mb_ld();
        if (s1 & 1u)
            continue;
        *out = g_pub;
        mb_ld();
        if (g_lockseq == s1)
            return 0;
    }
    *out = g_pub;
    return 0;
}

static void set_ret(hook_fargs4_t *args, long ret)
{
    args->ret = (uint64_t)ret;
    args->skip_origin = 1;
    if (has_syscall_wrapper) {
        struct pt_regs *regs = (struct pt_regs *)((hook_fargs0_t *)args)->args[0];
        if (regs)
            regs->regs[0] = (uint64_t)ret;
    }
}

static void emit(struct devst *d, unsigned int type, unsigned int code, int value)
{
    void (*fn)(void *, unsigned int, unsigned int, int);

    fn = (void *)g_input_fn;
    if (!fn || !d || !d->dev)
        return;
    g_injecting = 1;
    fn(d->dev, type, code, value);
    g_injecting = 0;
}

static struct devst *aim_dev(void)
{
    int i;

    for (i = 0; i < NDEV; i++) {
        if (g_dev[i].touch && g_dev[i].dev && g_dev[i].max_x > 100)
            return &g_dev[i];
    }
    return NULL;
}

static int user_holding(struct devst *d)
{
    int s;

    for (s = 0; s < NSLOT; s++) {
        if (s == 8)
            continue;
        if (d->f[s].id >= 0)
            return 1;
    }
    return 0;
}

static void aim_lift_dev(struct devst *d)
{
    if (!g_aim_down)
        return;
    emit(d, EV_ABS, ABS_MT_SLOT, 8);
    emit(d, EV_ABS, ABS_MT_TRACKING_ID, -1);
    if (!user_holding(d))
        emit(d, EV_KEY, BTN_TOUCH, 0);
    emit(d, EV_SYN, SYN_REPORT, 0);
    g_aim_down = 0;
}

static void aim_goto(struct devst *d, int x, int y)
{
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (d->max_x > 1 && x > d->max_x)
        x = d->max_x;
    if (d->max_y > 1 && y > d->max_y)
        y = d->max_y;
    emit(d, EV_ABS, ABS_MT_SLOT, 8);
    if (!g_aim_down) {
        emit(d, EV_ABS, ABS_MT_TRACKING_ID, 0x4B54);
        g_aim_down = 1;
        if (!user_holding(d))
            emit(d, EV_KEY, BTN_TOUCH, 1);
    }
    emit(d, EV_ABS, ABS_MT_POSITION_X, x);
    emit(d, EV_ABS, ABS_MT_POSITION_Y, y);
    emit(d, EV_SYN, SYN_REPORT, 0);
}

static void prctl_before(hook_fargs4_t *args, void *udata)
{
    int option;
    unsigned long uptr;
    struct ktouch_snap snap;

    (void)udata;
    option = (int)syscall_argn(args, 0);
    if (option != PR_KTOUCH_PING && option != PR_KTOUCH_GET && option != PR_KTOUCH_MOVE)
        return;
    if (!caller_ok()) {
        set_ret(args, EPERM_R);
        return;
    }
    if (option == PR_KTOUCH_PING) {
        set_ret(args, KTOUCH_PING_RET);
        return;
    }
    if (option == PR_KTOUCH_MOVE) {
        /* 不能在 prctl 钩子里再调用被挂钩的 input_event。
         * 上次这样打，system_server 在手势区域计算里崩了，整机软重启。
         * 自瞄改由悬浮窗直接写触摸设备节点。这里只拒绝。 */
        set_ret(args, EINVAL_R);
        return;
    }
    uptr = syscall_argn(args, 1);
    if (!uptr) {
        set_ret(args, EINVAL_R);
        return;
    }
    read_pub(&snap);
    /* compat_copy_to_user 成功时返回拷贝字节数，不是 0。 */
    if (compat_copy_to_user((void __user *)uptr, &snap, sizeof(snap)) != (int)sizeof(snap)) {
        set_ret(args, EFAULT_R);
        return;
    }
    set_ret(args, 0);
}

static unsigned long lookup_sym(const char *name)
{
    if (!name || !kallsyms_lookup_name)
        return 0;
    return kallsyms_lookup_name(name);
}

static int install_input(void)
{
    static const char *const names[] = {
        "input_event",
        "input_event.cfi_jt",
        "input_handle_event",
        "input_handle_event.cfi_jt",
    };
    int i;

    for (i = 0; i < 4; i++) {
        unsigned long addr = lookup_sym(names[i]);
        if (!addr)
            continue;
        if (hook_wrap4((void *)addr, input_before, NULL, NULL) == 0) {
            g_input_fn = (void *)addr;
            g_hooked_input = 1;
            pr_info("ktouch: hooked %s @ %lx\n", names[i], addr);
            return 0;
        }
        pr_warn("ktouch: hook %s failed\n", names[i]);
    }
    return -1;
}

long ktouch_init(const char *args, const char *event, void *__user reserved)
{
    int i, s;

    (void)reserved;
    kpm_demo_log_init("ktouch", event, args);

    for (i = 0; i < NDEV; i++) {
        g_dev[i].dev = NULL;
        g_dev[i].touch = 0;
        for (s = 0; s < NSLOT; s++)
            g_dev[i].f[s].id = -1;
    }
    g_pub.magic = KTOUCH_MAGIC;
    g_pub.seq = 0;
    g_pub.down = 0;
    g_lockseq = 0;
    g_evseq = 0;

    k_nofault = (void *)lookup_sym("copy_from_kernel_nofault");
    k_uidgid = (void *)lookup_sym("bpf_get_current_uid_gid");
    pr_info("ktouch: nofault=%px uidgid=%px\n", (void *)k_nofault, (void *)k_uidgid);

    if (install_input() < 0) {
        pr_err("ktouch: input_event hook failed\n");
        return -1;
    }
    if (inline_wrap_syscalln(__NR_prctl, 5, 0, prctl_before, NULL, NULL) == 0) {
        g_hooked_prctl = 1;
        pr_info("ktouch: prctl ready ping=0x%x get=0x%x\n",
                PR_KTOUCH_PING, PR_KTOUCH_GET);
    } else {
        pr_err("ktouch: prctl hook failed\n");
        hook_unwrap(g_input_fn, input_before, NULL);
        g_hooked_input = 0;
        g_input_fn = NULL;
        return -1;
    }
    return 0;
}

long ktouch_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[160];
    struct ktouch_snap s;

    (void)args;
    read_pub(&s);
    snprintf(buf, sizeof(buf),
             "down=%d seq=%u xy=%d,%d max=%d,%d slot=%d",
             s.down, s.seq, s.x, s.y, s.max_x, s.max_y, s.slot);
    return kpm_demo_copy_message(buf, out_msg, outlen);
}

long ktouch_exit(void *__user reserved)
{
    (void)reserved;
    if (g_hooked_prctl) {
        inline_unwrap_syscalln(__NR_prctl, 0, prctl_before, NULL);
        g_hooked_prctl = 0;
    }
    if (g_hooked_input && g_input_fn) {
        hook_unwrap(g_input_fn, input_before, NULL);
        g_hooked_input = 0;
        g_input_fn = NULL;
    }
    return kpm_demo_log_exit("ktouch");
}

#ifndef STEALTH_FUSED
KPM_INIT(ktouch_init);
KPM_CTL0(ktouch_control0);
KPM_EXIT(ktouch_exit);
#endif
