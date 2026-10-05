#define _GNU_SOURCE
#include "pack.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ADB_ROOT "/data/adb"

extern const unsigned char _binary_so_bin_start[];
extern const unsigned char _binary_so_bin_end[];
extern const unsigned char _binary_hud_bin_start[];
extern const unsigned char _binary_hud_bin_end[];
extern const unsigned char _binary_stealth_bin_start[];
extern const unsigned char _binary_stealth_bin_end[];
extern const unsigned char _binary_kpatch_bin_start[];
extern const unsigned char _binary_kpatch_bin_end[];

static const unsigned char kXor[] = {0xA7, 0x3C, 0x91, 0x5E};

enum {
	B_SO = 0,
	B_HUD,
	B_STEALTH,
	B_KPATCH,
	B_N
};

static void xor_buf(unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] ^= kXor[i & 3];
}

static int rand_bytes(void *p, size_t n)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	unsigned char *q = p;
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, q + got, n - got);
		if (r <= 0) {
			close(fd);
			return -1;
		}
		got += (size_t)r;
	}
	close(fd);
	return 0;
}

static void hex_encode(char *out, const unsigned char *in, size_t n)
{
	static const char *h = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[i * 2] = h[in[i] >> 4];
		out[i * 2 + 1] = h[in[i] & 0xF];
	}
	out[n * 2] = 0;
}

static int is_our_dir(const char *dir)
{
	const char *base;
	size_t n;
	if (!dir || strncmp(dir, ADB_ROOT "/.", 11) != 0)
		return 0;
	base = strrchr(dir, '/');
	if (!base || base[1] != '.')
		return 0;
	base++;
	n = strlen(base);
	if (n < 9 || n > 17)
		return 0;
	for (size_t i = 1; i < n; i++) {
		char c = base[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return 0;
	}
	return 1;
}

static int write_all(int fd, const unsigned char *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0)
			return -1;
		p += (size_t)w;
		n -= (size_t)w;
	}
	return 0;
}

static int write_blob(const char *path, const unsigned char *start,
		     const unsigned char *end, int mode)
{
	size_t n = (size_t)(end - start);
	unsigned char *buf;
	int fd;
	if (n == 0)
		return -1;
	buf = (unsigned char *)malloc(n);
	if (!buf)
		return -1;
	memcpy(buf, start, n);
	xor_buf(buf, n);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0) {
		free(buf);
		return -1;
	}
	if (write_all(fd, buf, n) != 0) {
		close(fd);
		free(buf);
		unlink(path);
		return -1;
	}
	fsync(fd);
	close(fd);
	free(buf);
	chmod(path, (mode_t)mode);
	return 0;
}

static int unique_hex8(char names[][16], int used, char *out)
{
	unsigned char raw[4];
	char hex[9];
	for (int t = 0; t < 16; t++) {
		int ok = 1;
		if (rand_bytes(raw, sizeof(raw)) != 0)
			return -1;
		hex_encode(hex, raw, sizeof(raw));
		for (int i = 0; i < used; i++) {
			if (strcmp(names[i], hex) == 0) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			snprintf(out, 16, "%s", hex);
			return 0;
		}
	}
	return -1;
}

int pack_extract(pack_paths *out)
{
	unsigned char raw[6];
	char hex[13], names[B_N][16];
	const unsigned char *start[B_N], *end[B_N];
	int mode[B_N];
	char *slot[B_N];

	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));
	memset(names, 0, sizeof(names));

	start[B_SO] = _binary_so_bin_start;
	end[B_SO] = _binary_so_bin_end;
	mode[B_SO] = 0755;
	slot[B_SO] = out->so;

	start[B_HUD] = _binary_hud_bin_start;
	end[B_HUD] = _binary_hud_bin_end;
	mode[B_HUD] = 0755;
	slot[B_HUD] = out->hud;

	start[B_STEALTH] = _binary_stealth_bin_start;
	end[B_STEALTH] = _binary_stealth_bin_end;
	mode[B_STEALTH] = 0600;
	slot[B_STEALTH] = out->stealth;

	start[B_KPATCH] = _binary_kpatch_bin_start;
	end[B_KPATCH] = _binary_kpatch_bin_end;
	mode[B_KPATCH] = 0755;
	slot[B_KPATCH] = out->kpatch;

	mkdir(ADB_ROOT, 0700);
	{
		int made = 0;
		for (int t = 0; t < 8; t++) {
			if (rand_bytes(raw, sizeof(raw)) != 0)
				return -1;
			hex_encode(hex, raw, sizeof(raw));
			snprintf(out->dir, sizeof(out->dir), "%s/.%s", ADB_ROOT, hex);
			if (mkdir(out->dir, 0700) == 0) {
				made = 1;
				break;
			}
			if (errno != EEXIST) {
				fprintf(stderr, "[-] mkdir %s errno=%d\n", out->dir, errno);
				return -1;
			}
		}
		if (!made)
			return -1;
	}

	for (int i = 0; i < B_N; i++) {
		if (unique_hex8(names, i, names[i]) != 0) {
			pack_wipe(out->dir);
			return -1;
		}
		snprintf(slot[i], 160, "%s/%s", out->dir, names[i]);
		if (write_blob(slot[i], start[i], end[i], mode[i]) != 0) {
			fprintf(stderr, "[-] extract %d errno=%d\n", i, errno);
			pack_wipe(out->dir);
			return -1;
		}
	}
	return 0;
}

void pack_wipe(const char *dir)
{
	DIR *d;
	struct dirent *e;
	if (!is_our_dir(dir))
		return;
	d = opendir(dir);
	if (d) {
		while ((e = readdir(d))) {
			char path[192];
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
			unlink(path);
		}
		closedir(d);
	}
	rmdir(dir);
}
