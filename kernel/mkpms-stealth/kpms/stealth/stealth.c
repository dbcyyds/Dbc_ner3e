/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * stealth.kpm — 唯一加载镜像
 *
 *   - kfix:      串行 write/remove_kstorage，失败不回滚
 *   - hide-maps: maps/smaps/map_files 隐藏 + prctl
 *   - kload:     无 ptrace dlopen 劫持
 *   - wxshadow:  W^X shadow BP/PATCH + prctl
 *   - dbc-rw:    跨进程内存读写（syscall 41 + magic）
 *   - nr3ehid:   socket(198)+magic 邮箱（HUD↔SO，无 getdents）
 *   - kpcloak:   代码在镜像里，init 不调用。上次开机后整机重启，先不武装
 *   - ktouch:    代码在镜像里，init 不调用。input_event 钩子会软重启
 *
 * Usage:
 *   kpatch <key> kpm load stealth.kpm
 */

#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <kputils.h>
#include "../common/kpm_demo_helpers.h"

KPM_MODULE_INFO("stealth",
                "1.3.2",
                "GPL v2",
                "dbc",
                "Unified: kfix hide-maps kload wxshadow dbc-rw mailbox kpcloak; ktouch linked but not armed");

/* 子模块导出（STEALTH_FUSED） */
long hide_maps_init(const char *args, const char *event, void *__user reserved);
long hide_maps_exit(void *__user reserved);
long hide_maps_control0(const char *args, char *__user out_msg, int outlen);

long kload_init(const char *args, const char *event, void *__user reserved);
long kload_exit(void *__user reserved);
long kload_control0(const char *args, char *__user out_msg, int outlen);

long wxshadow_init(const char *args, const char *event, void *__user reserved);
long wxshadow_exit(void *__user reserved);
long wxshadow_control(const char *args, char *__user out_msg, int outlen);

long dbc_rw_init(const char *args, const char *event, void *__user reserved);
long dbc_rw_exit(void *__user reserved);
long dbc_rw_control0(const char *args, char *__user out_msg, int outlen);

long nr3ehid_init(const char *args, const char *event, void *__user reserved);
long nr3ehid_exit(void *__user reserved);
long nr3ehid_ctl0(const char *args, char *__user out_msg, int outlen);

long kfix_init(const char *args, const char *event, void *__user reserved);
long kfix_exit(void *__user reserved);

long kpcloak_init(const char *args, const char *event, void *__user reserved);
long kpcloak_exit(void *__user reserved);
long kpcloak_control(const char *args, char *__user out_msg, int outlen);

/* 链进镜像，禁止从 stealth_init 调用。input_event 钩子曾经把这台平板打软重启。 */
long ktouch_init(const char *args, const char *event, void *__user reserved);
long ktouch_exit(void *__user reserved);
long ktouch_control0(const char *args, char *__user out_msg, int outlen);

__attribute__((used))
static void *const g_ktouch_keep[] = {
    (void *)ktouch_init,
    (void *)ktouch_exit,
    (void *)ktouch_control0,
};

static int g_kfix_ok;
static int g_hm_ok;
static int g_kl_ok;
static int g_wx_ok;
static int g_rw_ok;
static int g_mb_ok;
static int g_cloak_ok;

/*
 * 初始化顺序：
 *   1) kfix      — 失败不回滚
 *   2) hide-maps
 *   3) kload
 *   4) wxshadow
 *   5) dbc-rw    — 失败不回滚
 *   6) nr3ehid   — 失败不回滚
 *   7) kpcloak   — 失败不回滚
 * ktouch_init 不在此列。
 */
static long stealth_init(const char *args, const char *event, void *__user reserved)
{
    long rc;

    (void)g_ktouch_keep;
    pr_info("stealth: init begin (kfix+hm+kload+wx+rw+mb+cloak), args=%s\n",
            args ? args : "(null)");

    g_kfix_ok = g_hm_ok = g_kl_ok = g_wx_ok = g_rw_ok = g_mb_ok = g_cloak_ok = 0;

    rc = kfix_init(args, event, reserved);
    if (rc) {
        pr_warn("stealth: kfix init failed rc=%ld (continue)\n", rc);
        g_kfix_ok = 0;
    } else {
        g_kfix_ok = 1;
        pr_info("stealth: [1/7] kfix ready\n");
    }

    rc = hide_maps_init(args, event, reserved);
    if (rc) {
        pr_err("stealth: hide-maps init failed rc=%ld\n", rc);
        if (g_kfix_ok) {
            kfix_exit(reserved);
            g_kfix_ok = 0;
        }
        return rc;
    }
    g_hm_ok = 1;
    pr_info("stealth: [2/7] hide-maps ready\n");

    rc = kload_init(args, event, reserved);
    if (rc) {
        pr_err("stealth: kload init failed rc=%ld\n", rc);
        hide_maps_exit(reserved);
        g_hm_ok = 0;
        if (g_kfix_ok) {
            kfix_exit(reserved);
            g_kfix_ok = 0;
        }
        return rc;
    }
    g_kl_ok = 1;
    pr_info("stealth: [3/7] kload ready\n");

    rc = wxshadow_init(args, event, reserved);
    if (rc) {
        pr_err("stealth: wxshadow init failed rc=%ld\n", rc);
        kload_exit(reserved);
        hide_maps_exit(reserved);
        g_kl_ok = g_hm_ok = 0;
        if (g_kfix_ok) {
            kfix_exit(reserved);
            g_kfix_ok = 0;
        }
        return rc;
    }
    g_wx_ok = 1;
    pr_info("stealth: [4/7] wxshadow ready\n");

    rc = dbc_rw_init(args, event, reserved);
    if (rc) {
        pr_warn("stealth: dbc-rw init failed rc=%ld (continue without r/w)\n", rc);
        g_rw_ok = 0;
    } else {
        g_rw_ok = 1;
        pr_info("stealth: [5/7] dbc-rw ready (syscall 41)\n");
    }

    rc = nr3ehid_init(args, event, reserved);
    if (rc) {
        pr_warn("stealth: mailbox init failed rc=%ld (HUD IPC unavailable)\n", rc);
        g_mb_ok = 0;
    } else {
        g_mb_ok = 1;
        pr_info("stealth: [6/7] mailbox ready (socket 198)\n");
    }

    g_cloak_ok = 0;
    (void)kpcloak_init;

    pr_info("stealth: all up kfix=%d hm=%d kl=%d wx=%d rw=%d mb=%d cloak=0 touch=0\n",
            g_kfix_ok, g_hm_ok, g_kl_ok, g_wx_ok, g_rw_ok, g_mb_ok);
    return 0;
}

static long stealth_exit(void *__user reserved)
{
    pr_info("stealth: exit (reverse order)\n");
    if (g_cloak_ok) {
        kpcloak_exit(reserved);
        g_cloak_ok = 0;
    }
    if (g_mb_ok) {
        nr3ehid_exit(reserved);
        g_mb_ok = 0;
    }
    if (g_rw_ok) {
        dbc_rw_exit(reserved);
        g_rw_ok = 0;
    }
    if (g_wx_ok) {
        wxshadow_exit(reserved);
        g_wx_ok = 0;
    }
    if (g_kl_ok) {
        kload_exit(reserved);
        g_kl_ok = 0;
    }
    if (g_hm_ok) {
        hide_maps_exit(reserved);
        g_hm_ok = 0;
    }
    if (g_kfix_ok) {
        kfix_exit(reserved);
        g_kfix_ok = 0;
    }
    return kpm_demo_log_exit("stealth");
}

/*
 * ctl0：
 *   hide ... | kload ... | wx ... | rw ... | mbox ... | cloak ... | touch | 空=状态
 * touch 只回报未武装，不调用 ktouch_init。
 */
static long stealth_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[512];

    if (args && !strncmp(args, "hide", 4))
        return hide_maps_control0(args + 4 + (args[4] == ':' || args[4] == ' '),
                                  out_msg, outlen);
    if (args && !strncmp(args, "kload", 5))
        return kload_control0(args + 5 + (args[5] == ':' || args[5] == ' '),
                              out_msg, outlen);
    if (args && (!strncmp(args, "wx", 2) || !strncmp(args, "wxshadow", 8))) {
        const char *p = args;
        if (!strncmp(p, "wxshadow", 8))
            p += 8;
        else
            p += 2;
        if (*p == ':' || *p == ' ')
            p++;
        return wxshadow_control(p, out_msg, outlen);
    }
    if (args && (!strncmp(args, "rw", 2) || !strncmp(args, "dbc", 3))) {
        const char *p = args;
        if (!strncmp(p, "dbc-rw", 6))
            p += 6;
        else if (!strncmp(p, "dbc", 3))
            p += 3;
        else
            p += 2;
        if (*p == ':' || *p == ' ')
            p++;
        return dbc_rw_control0(p, out_msg, outlen);
    }
    if (args && (!strncmp(args, "mbox", 4) || !strncmp(args, "hid", 3) ||
                 !strncmp(args, "nr3e", 4))) {
        const char *p = args;
        if (!strncmp(p, "mbox", 4))
            p += 4;
        else if (!strncmp(p, "nr3ehid", 7))
            p += 7;
        else if (!strncmp(p, "nr3e", 4))
            p += 4;
        else
            p += 3;
        if (*p == ':' || *p == ' ')
            p++;
        return nr3ehid_ctl0(p, out_msg, outlen);
    }
    if (args && (!strncmp(args, "cloak", 5) || !strncmp(args, "kpcloak", 7))) {
        const char *p = args;
        if (!strncmp(p, "kpcloak", 7))
            p += 7;
        else
            p += 5;
        if (*p == ':' || *p == ' ')
            p++;
        return kpcloak_control(p, out_msg, outlen);
    }
    if (args && !strncmp(args, "touch", 5)) {
        snprintf(buf, sizeof(buf), "touch=0 linked not armed");
        return kpm_demo_copy_message(buf, out_msg, outlen);
    }

    snprintf(buf, sizeof(buf),
             "stealth kfix=%d hm=%d kl=%d wx=%d rw=%d mb=%d cloak=%d touch=0 | ctl: hide|kload|wx|rw|mbox|cloak|touch",
             g_kfix_ok, g_hm_ok, g_kl_ok, g_wx_ok, g_rw_ok, g_mb_ok, g_cloak_ok);
    return kpm_demo_copy_message(buf, out_msg, outlen);
}

KPM_INIT(stealth_init);
KPM_CTL0(stealth_control0);
KPM_EXIT(stealth_exit);
