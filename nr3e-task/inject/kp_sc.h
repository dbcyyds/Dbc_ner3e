#pragma once

/*
 * KernelPatch SuperCall (syscall 45) — load KPM without exec'ing kpatch.
 * Embedded kpatch is 4K-aligned and will not run on 16K-page devices.
 */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#define KP_NR_SUPERCALL 45
#define KP_HELLO 0x1000
#define KP_HELLO_MAGIC 0x11581158L
#define KP_KPVER 0x1008
#define KP_KPM_LOAD 0x1020
#define KP_KPM_LIST 0x1031
#define KP_KPM_INFO 0x1032

/* 0 = version encoding (KP >= 0xa05), 1 = hash encoding (older) */
static int kp_enc;

static inline long kp_ver_cmd(long cmd)
{
	return ((long)0x00000d00 << 32) | (0x1158L << 16) | (cmd & 0xFFFF);
}

static inline long kp_hash_cmd(const char *key, long cmd)
{
	long hash = 1000000007;
	int i;

	for (i = 0; key && key[i]; i++)
		hash = hash * 31 + (unsigned char)key[i];
	return (hash & 0xFFFF0000L) | (cmd & 0xFFFF);
}

static inline long kp_sc5(const char *key, long cmd, long a1, long a2, long a3, long a4)
{
	long c = (kp_enc == 1) ? kp_hash_cmd(key, cmd) : kp_ver_cmd(cmd);
	return syscall(KP_NR_SUPERCALL, key, c, a1, a2, a3, a4);
}

static inline long kp_hello(const char *key)
{
	long r;

	if (!key || !key[0])
		return -EINVAL;
	r = syscall(KP_NR_SUPERCALL, key, kp_ver_cmd(KP_HELLO));
	if (r == KP_HELLO_MAGIC) {
		kp_enc = 0;
		return r;
	}
	r = syscall(KP_NR_SUPERCALL, key, kp_hash_cmd(key, KP_HELLO));
	if (r == KP_HELLO_MAGIC) {
		kp_enc = 1;
		return r;
	}
	return r;
}

static inline int kp_ready(const char *key)
{
	return kp_hello(key) == KP_HELLO_MAGIC;
}

static inline long kp_ver(const char *key)
{
	return kp_sc5(key, KP_KPVER, 0, 0, 0, 0);
}

static inline long kp_kpm_load(const char *key, const char *path)
{
	if (!path || !path[0])
		return -EINVAL;
	return kp_sc5(key, KP_KPM_LOAD, (long)path, 0, 0, 0);
}

static inline long kp_kpm_list(const char *key, char *buf, int n)
{
	if (!buf || n <= 0)
		return -EINVAL;
	return kp_sc5(key, KP_KPM_LIST, (long)buf, n, 0, 0);
}
