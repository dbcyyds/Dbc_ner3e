#pragma once

#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>

/*
 * nr3ehid.kpm — syscall(SYS_socket=198 on aarch64) + magic。
 * 不用 41：aarch64 的 41 是 pivot_root。
 */
#ifndef SYS_socket
#define SYS_socket 198
#endif
#define NR3E_SYSNO     SYS_socket
#define NR3E_XMAGIC    0x4E33C0DE5A1E0000ULL
#define NR3E_CMD_PING  0
#define NR3E_CMD_BIND  1
#define NR3E_CMD_POST  2
#define NR3E_CMD_TAKE  3
#define NR3E_CMD_REPLY 4
#define NR3E_CMD_POLL  5
#define NR3E_CMD_HIDETID 6
#define NR3E_CMD_HIDEPID 7
#define NR3E_MAGIC     0x4E33

struct nr3e_take
{
	int32_t cmd;
	int32_t extra;
};

struct nr3e_kreply
{
	int32_t enabled;
	int32_t touched;
	char error[192];
	char message[192];
};

static inline long nr3e_xcall(unsigned cmd, unsigned long a1, unsigned long a2,
	unsigned long a3, unsigned long a4)
{
	return syscall(NR3E_SYSNO, (long)(NR3E_XMAGIC | (unsigned long)cmd), (long)a1,
		(long)a2, (long)a3, (long)a4);
}

static inline long nr3e_kping(void)
{
	return nr3e_xcall(NR3E_CMD_PING, 0, 0, 0, 0);
}
