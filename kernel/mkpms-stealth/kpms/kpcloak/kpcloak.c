/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * kpcloak — close the two userspace root tells measured on this device:
 *
 *   stat("/system/bin/su") is rewritten onto /system/bin/sh for an allowlisted
 *   uid, so the probe sees sh's inode and size while nlink stays 1, and
 *   open("/system/bin/su") still returns ENOENT.
 *   stat("/data/adb") returns EACCES, which proves the directory exists.
 *
 * execve of /system/bin/su is left alone, so an allowlisted shell can still
 * become uid 0. Path probes themselves return ENOENT.
 */

#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <uapi/asm-generic/unistd.h>
#include <uapi/asm-generic/errno-base.h>
#include <syscall.h>
#include <kputils.h>
#include <asm/ptrace.h>

#ifndef STEALTH_FUSED
KPM_NAME("kpcloak");
KPM_VERSION("1.0.1");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("kpcloak");
KPM_DESCRIPTION("ENOENT for the su stat alias and for /data/adb outside uid 0 and the manager");
#endif

/* dbc.ap on this tablet. ctl0 "exempt UID" adds more. */
#define MANAGER_UID 10231

#define EXEMPT_MAX 8
#define PATH_MAX_LOCAL 256

static uid_t extra_exempt[EXEMPT_MAX];
static int extra_exempt_n;
static int hooks_on;
static unsigned long su_hides;
static unsigned long adb_hides;

struct cloak_hook {
    int nr;
    int narg;
    int armed;
};

static struct cloak_hook cloak_hooks[] = {
    { __NR3264_fstatat, 4, 0 },
    { __NR_faccessat, 3, 0 },
    { __NR_faccessat2, 4, 0 },
    { __NR_statx, 5, 0 },
    { __NR_openat, 4, 0 },
    { __NR_openat2, 4, 0 },
};

/* 32-bit compat: fstatat64 and faccessat. sucompat rewrites these too. */
static struct cloak_hook compat_hooks[] = {
    { 327, 4, 0 },
    { 334, 3, 0 },
};

static int path_is_su(const char *p)
{
    return !strcmp(p, "/system/bin/su") || !strcmp(p, "/system/bin/kp");
}

static int path_is_adb(const char *p)
{
    static const char pre[] = "/data/adb";
    int i;

    for (i = 0; pre[i]; i++) {
        if (p[i] != pre[i])
            return 0;
    }
    return p[i] == '\0' || p[i] == '/';
}

/* sucompat's fstatat/faccessat hook replaces the user pointer with a copy of
 * "/system/bin/sh" placed just below the user stack pointer. A real stat of
 * sh does not pass a pointer in that slot. */
static int path_is_rewritten_sh(void *fargs, const char __user *up, const char *p)
{
    struct pt_regs *regs;
    unsigned long sp, u;

    if (strcmp(p, "/system/bin/sh"))
        return 0;
    if (!has_syscall_wrapper)
        return 0;
    regs = (struct pt_regs *)((hook_fargs0_t *)fargs)->args[0];
    if (!regs)
        return 0;
    sp = regs->sp;
    u = (unsigned long)up;
    if (!sp || u >= sp)
        return 0;
    return (sp - u) <= 32;
}

static int adb_visible(uid_t uid)
{
    int i;

    if (uid == 0 || uid == MANAGER_UID)
        return 1;
    for (i = 0; i < extra_exempt_n; i++) {
        if (extra_exempt[i] == uid)
            return 1;
    }
    return 0;
}

static void cloak_before(hook_fargs4_t *args, void *udata)
{
    const char __user *up;
    char buf[PATH_MAX_LOCAL];
    long n;
    uid_t uid;
    int su, adb, rewritten;

    (void)udata;
    up = (const char __user *)syscall_argn(args, 1);
    if (!up)
        return;

    n = compat_strncpy_from_user(buf, up, sizeof(buf));
    if (n <= 0)
        return;
    buf[sizeof(buf) - 1] = '\0';
    if (buf[0] == '\0')
        return;

    su = path_is_su(buf);
    rewritten = path_is_rewritten_sh(args, up, buf);
    adb = path_is_adb(buf);
    if (!su && !rewritten && !adb)
        return;

    if (adb) {
        uid = current_uid();
        if (adb_visible(uid))
            return;
        adb_hides++;
    } else {
        su_hides++;
    }

    args->skip_origin = 1;
    args->ret = (uint64_t)(-ENOENT);
}

static void disarm(void)
{
    int i;

    if (!hooks_on)
        return;
    for (i = 0; i < (int)(sizeof(cloak_hooks) / sizeof(cloak_hooks[0])); i++) {
        if (!cloak_hooks[i].armed)
            continue;
        unhook_syscalln(cloak_hooks[i].nr, cloak_before, 0);
        cloak_hooks[i].armed = 0;
    }
    for (i = 0; i < (int)(sizeof(compat_hooks) / sizeof(compat_hooks[0])); i++) {
        if (!compat_hooks[i].armed)
            continue;
        unhook_compat_syscalln(compat_hooks[i].nr, cloak_before, 0);
        compat_hooks[i].armed = 0;
    }
    hooks_on = 0;
}

static int arm(void)
{
    int i, rc, any = 0;

    for (i = 0; i < (int)(sizeof(cloak_hooks) / sizeof(cloak_hooks[0])); i++) {
        rc = hook_syscalln(cloak_hooks[i].nr, cloak_hooks[i].narg, cloak_before, 0, 0);
        if (rc) {
            pr_info("kpcloak: hook %d failed %d\n", cloak_hooks[i].nr, rc);
            cloak_hooks[i].armed = 0;
            continue;
        }
        cloak_hooks[i].armed = 1;
        any = 1;
    }
    for (i = 0; i < (int)(sizeof(compat_hooks) / sizeof(compat_hooks[0])); i++) {
        rc = hook_compat_syscalln(compat_hooks[i].nr, compat_hooks[i].narg, cloak_before, 0, 0);
        if (rc) {
            pr_info("kpcloak: compat hook %d failed %d\n", compat_hooks[i].nr, rc);
            compat_hooks[i].armed = 0;
            continue;
        }
        compat_hooks[i].armed = 1;
        any = 1;
    }
    hooks_on = any;
    return any ? 0 : -ENODEV;
}

static int exempt_add(uid_t uid)
{
    int i;

    if (uid == 0)
        return 0;
    for (i = 0; i < extra_exempt_n; i++) {
        if (extra_exempt[i] == uid)
            return 0;
    }
    if (extra_exempt_n >= EXEMPT_MAX)
        return -ENOMEM;
    extra_exempt[extra_exempt_n++] = uid;
    return 0;
}

static int exempt_del(uid_t uid)
{
    int i;

    for (i = 0; i < extra_exempt_n; i++) {
        if (extra_exempt[i] != uid)
            continue;
        extra_exempt[i] = extra_exempt[extra_exempt_n - 1];
        extra_exempt_n--;
        return 0;
    }
    return -ENOENT;
}

static unsigned int parse_u32(const char *s, int *ok)
{
    unsigned int v = 0;

    *ok = 0;
    if (!s || !*s)
        return 0;
    while (*s) {
        if (*s < '0' || *s > '9')
            return 0;
        v = v * 10u + (unsigned int)(*s - '0');
        s++;
    }
    *ok = 1;
    return v;
}

static const char *skip_word(const char *s)
{
    while (*s && *s != ' ')
        s++;
    while (*s == ' ')
        s++;
    return s;
}

long kpcloak_control(const char *args, char *__user out_msg, int outlen)
{
    char msg[192];
    int n = 0;
    int ok;
    unsigned int uid;
    const char *rest;
    if (!args)
        args = "";

    if (!strcmp(args, "status") || args[0] == '\0') {
        n = snprintf(msg, sizeof(msg),
                     "hooks=%d su=%lu adb=%lu mgr=%u exempt=%d\n",
                     hooks_on, su_hides, adb_hides,
                     (unsigned int)MANAGER_UID, extra_exempt_n);
    } else if (!strncmp(args, "exempt ", 7)) {
        rest = skip_word(args);
        uid = parse_u32(rest, &ok);
        if (!ok) {
            n = snprintf(msg, sizeof(msg), "exempt needs a uid\n");
        } else {
            exempt_add((uid_t)uid);
            n = snprintf(msg, sizeof(msg), "exempt %u\n", uid);
        }
    } else if (!strncmp(args, "unexempt ", 9)) {
        rest = skip_word(args);
        uid = parse_u32(rest, &ok);
        if (!ok) {
            n = snprintf(msg, sizeof(msg), "unexempt needs a uid\n");
        } else {
            exempt_del((uid_t)uid);
            n = snprintf(msg, sizeof(msg), "unexempt %u rc done\n", uid);
        }
    } else {
        n = snprintf(msg, sizeof(msg), "status | exempt UID | unexempt UID\n");
    }

    if (n < 0)
        return n;
    if (!out_msg || outlen <= 0)
        return 0;
    if (n >= outlen)
        n = outlen - 1;
    return compat_copy_to_user(out_msg, msg, n + 1);
}

long kpcloak_init(const char *args, const char *event, void *__user reserved)
{
    int rc;

    (void)args;
    (void)reserved;
    pr_info("kpcloak init event=%s\n", event ? event : "");
    rc = arm();
    pr_info("kpcloak arm rc=%d\n", rc);
    return rc;
}

long kpcloak_exit(void *__user reserved)
{
    (void)reserved;
    disarm();
    pr_info("kpcloak exit\n");
    return 0;
}

#ifndef STEALTH_FUSED
KPM_INIT(kpcloak_init);
KPM_CTL0(kpcloak_control);
KPM_EXIT(kpcloak_exit);
#endif
