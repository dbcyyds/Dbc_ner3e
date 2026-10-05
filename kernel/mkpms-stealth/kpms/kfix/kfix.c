/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Serialize write_kstorage and remove_kstorage.
 *
 * The running image finds the old hlist node, then takes kstorage_glocks,
 * then hlist_replace_rcu. remove_kstorage can hlist_del_rcu that node in
 * between and set pprev to LIST_POISON2. The replace store-releases through
 * that pointer (data abort, comm apd). The buckets are private to the image,
 * so the live fix is one lock around both calls. Same task may re-enter
 * while vmalloc sleeps; depth covers that. A non-owner waits with schedule
 * so the lock is not held as a spin across vmalloc.
 */
#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <common.h>
#include <hook.h>
#include <kallsyms.h>
#include <asm/current.h>
#include "../common/kpm_demo_helpers.h"

#ifndef STEALTH_FUSED
KPM_MODULE_INFO("kfix",
                "1.0.0",
                "GPL v2",
                "dbc",
                "Serialize kstorage write and remove");
#endif

static int g_held;
static struct task_struct *g_owner;
static int g_depth;
static void (*k_schedule)(void);
static void *g_write;
static void *g_remove;
static int g_write_hooked;
static int g_remove_hooked;

/* Names are resolved by the loader from the KP export table. */
extern void write_kstorage(void);
extern void remove_kstorage(void);

static void kfix_enter(void)
{
    struct task_struct *me = current;

    if (__atomic_load_n(&g_held, __ATOMIC_RELAXED) &&
        __atomic_load_n(&g_owner, __ATOMIC_RELAXED) == me) {
        g_depth++;
        return;
    }
    for (;;) {
        int zero = 0;

        if (__atomic_compare_exchange_n(&g_held, &zero, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            g_owner = me;
            g_depth = 1;
            return;
        }
        if (k_schedule)
            k_schedule();
        else
            __asm__ volatile("yield" ::: "memory");
    }
}

static void kfix_leave(void)
{
    if (g_owner == current && g_depth > 1) {
        g_depth--;
        return;
    }
    g_depth = 0;
    __atomic_store_n(&g_owner, (struct task_struct *)0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_held, 0, __ATOMIC_RELEASE);
}

static void kfix_before6(hook_fargs6_t *args, void *udata)
{
    (void)args;
    (void)udata;
    kfix_enter();
}

static void kfix_after6(hook_fargs6_t *args, void *udata)
{
    (void)args;
    (void)udata;
    kfix_leave();
}

static void kfix_before2(hook_fargs2_t *args, void *udata)
{
    (void)args;
    (void)udata;
    kfix_enter();
}

static void kfix_after2(hook_fargs2_t *args, void *udata)
{
    (void)args;
    (void)udata;
    kfix_leave();
}

long kfix_init(const char *args, const char *event, void *__user reserved)
{
    hook_err_t err;

    (void)args;
    (void)event;
    (void)reserved;

    if (kallsyms_lookup_name)
        k_schedule = (void (*)(void))kallsyms_lookup_name("schedule");

    g_write = (void *)write_kstorage;
    g_remove = (void *)remove_kstorage;
    pr_info("kfix: write=%px remove=%px schedule=%px\n",
            g_write, g_remove, k_schedule);
    if (!g_write || !g_remove) {
        pr_err("kfix: kstorage symbols missing\n");
        return -1;
    }
    err = hook_wrap6(g_write, kfix_before6, kfix_after6, NULL);
    if (err != HOOK_NO_ERR) {
        pr_err("kfix: hook write_kstorage failed %d\n", (int)err);
        g_write = NULL;
        return -1;
    }
    g_write_hooked = 1;

    err = hook_wrap2(g_remove, kfix_before2, kfix_after2, NULL);
    if (err != HOOK_NO_ERR) {
        pr_err("kfix: hook remove_kstorage failed %d\n", (int)err);
        hook_unwrap(g_write, kfix_before6, kfix_after6);
        g_write_hooked = 0;
        g_write = NULL;
        g_remove = NULL;
        return -1;
    }
    g_remove_hooked = 1;
    pr_info("kfix: kstorage write/remove serialized\n");
    return 0;
}

long kfix_exit(void *__user reserved)
{
    int i;

    (void)reserved;
    for (i = 0; i < 200 && __atomic_load_n(&g_held, __ATOMIC_ACQUIRE); i++) {
        if (k_schedule)
            k_schedule();
        else
            __asm__ volatile("yield" ::: "memory");
    }
    if (g_write_hooked && g_write) {
        hook_unwrap(g_write, kfix_before6, kfix_after6);
        g_write_hooked = 0;
    }
    if (g_remove_hooked && g_remove) {
        hook_unwrap(g_remove, kfix_before2, kfix_after2);
        g_remove_hooked = 0;
    }
    return 0;
}

#ifndef STEALTH_FUSED
KPM_INIT(kfix_init);
KPM_EXIT(kfix_exit);
#endif
