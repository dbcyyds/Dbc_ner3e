#define _GNU_SOURCE
#include "pack.h"
#include "stealth_inject.h"
#include "stealth_inject_api.h"
#include "nr3e/kpm.h"
#include "kp_sc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static const char *g_pkg = "com.tencent.letsgo";
static const char *g_activity = "com.epicgames.ue4.GameActivityExt";
static char g_keybuf[256];
static const char *g_key;
static int g_restart;
static int g_no_hud;
static int g_timeout = 90;
static int g_delay_ms = 2000;
static long g_rss_kb = 500000;
static pack_paths g_pack;

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--restart] [--no-hud] [--key KEY] [-p PKG]\n"
		"  extract hud/kpm/so → load kpm → start game → inject → wipe\n",
		prog);
}

static void wipe_now(void)
{
	if (g_pack.dir[0]) {
		pack_wipe(g_pack.dir);
		g_pack.dir[0] = 0;
	}
}

static void on_sig(int sig)
{
	wipe_now();
	_exit(128 + sig);
}

static int prompt_key(char *buf, size_t n)
{
	int tty = open("/dev/tty", O_RDWR);
	int fd = tty >= 0 ? tty : STDIN_FILENO;
	struct termios oldt, newt;
	int restore = 0;
	char *nl;
	ssize_t r;
	size_t got = 0;

	if (tcgetattr(fd, &oldt) == 0) {
		newt = oldt;
		newt.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
		if (tcsetattr(fd, TCSANOW, &newt) == 0)
			restore = 1;
	}
	if (write(fd, "APatch key: ", 12) < 0 && tty < 0)
		fprintf(stderr, "APatch key: ");
	memset(buf, 0, n);
	while (got + 1 < n) {
		char c;
		r = read(fd, &c, 1);
		if (r <= 0)
			break;
		if (c == '\n' || c == '\r')
			break;
		buf[got++] = c;
	}
	buf[got] = 0;
	if (restore)
		tcsetattr(fd, TCSANOW, &oldt);
	if (write(fd, "\n", 1) < 0)
		fputc('\n', stderr);
	if (tty >= 0)
		close(tty);
	nl = strchr(buf, '\n');
	if (nl)
		*nl = 0;
	nl = strchr(buf, '\r');
	if (nl)
		*nl = 0;
	return buf[0] ? 0 : -1;
}

static pid_t find_pkg(const char *pkg)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	pid_t found = 0;
	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char path[64], buf[256];
		int fd, n;
		char *end = NULL;
		pid_t pid;
		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		pid = (pid_t)strtol(e->d_name, &end, 10);
		if (!end || *end || pid <= 1)
			continue;
		snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		n = (int)read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = 0;
		if (strcmp(buf, pkg) == 0) {
			found = pid;
			break;
		}
	}
	closedir(d);
	return found;
}

static long proc_rss_kb(pid_t pid)
{
	char path[64], line[128];
	FILE *fp;
	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		long v = 0;
		if (sscanf(line, "VmRSS: %ld", &v) == 1) {
			fclose(fp);
			return v;
		}
	}
	fclose(fp);
	return 0;
}

static int hide_ping(void)
{
	return prctl(PR_HIDEMAPS_PING, 0, 0, 0, 0) == HIDEMAPS_MAGIC;
}

static int ensure_kpm(const pack_paths *p)
{
	long hello, ver, rc;
	char list[256];

	if (hide_ping() && nr3e_kping() == NR3E_MAGIC) {
		printf("[+] kpm already ready\n");
		return 0;
	}

	printf("[*] kpm load stealth\n");
	hello = kp_hello(g_key);
	if (hello != KP_HELLO_MAGIC) {
		fprintf(stderr,
			"[-] KernelPatch hello failed rc=%ld (not 0x11581158)\n"
			"    这台机内核里没有 APatch/KernelPatch，或超级密钥不对。\n"
			"    Magisk/KernelSU 不够；密钥必须是【本机】刷 APatch 时设的，旧机密钥不能用。\n",
			hello);
		return -1;
	}
	ver = kp_ver(g_key);
	printf("[+] KernelPatch hello ok kpver=0x%lx\n", ver);

	if (!hide_ping()) {
		if (access(p->stealth, R_OK) != 0) {
			fprintf(stderr, "[-] kpm missing: %s errno=%d\n", p->stealth, errno);
			return -1;
		}
		rc = kp_kpm_load(g_key, p->stealth);
		memset(list, 0, sizeof(list));
		kp_kpm_list(g_key, list, (int)sizeof(list) - 1);
		printf("[*] supercall kpm load rc=%ld list=[%s]\n", rc, list);
		if (rc != 0) {
			fprintf(stderr,
				"[-] kpm load failed rc=%ld path=%s\n"
				"    dmesg | grep -iE 'stealth|hide-maps|kpm|KP '\n",
				rc, p->stealth);
			return -1;
		}
		usleep(250 * 1000);
	}

	if (!hide_ping()) {
		fprintf(stderr,
			"[-] stealth.kpm loaded but hide ping failed (reboot then run again)\n");
		return -1;
	}
	if (nr3e_kping() != NR3E_MAGIC) {
		fprintf(stderr, "[-] mailbox not ready (need fused stealth 1.2, reboot then run)\n");
		return -1;
	}
	printf("[+] kpm ready\n");
	return 0;
}

static void start_game(int force)
{
	char cmd[384];
	pid_t pid = find_pkg(g_pkg);
	if (force && pid > 0) {
		printf("[*] force-stop %s\n", g_pkg);
		snprintf(cmd, sizeof(cmd), "am force-stop %s >/dev/null 2>&1", g_pkg);
		system(cmd);
		sleep(1);
		pid = 0;
	}
	if (pid <= 0) {
		printf("[*] start %s\n", g_pkg);
		snprintf(cmd, sizeof(cmd), "am start -n %s/%s >/dev/null 2>&1", g_pkg,
			 g_activity);
		system(cmd);
	}
}

static pid_t wait_engine(void)
{
	int max = g_timeout / 2;
	if (max < 8)
		max = 8;
	for (int i = 0; i < max; i++) {
		pid_t pid = find_pkg(g_pkg);
		if (pid > 0) {
			long rss = proc_rss_kb(pid);
			printf("[*] t=%d pid=%d rss=%ldkB\n", i, (int)pid, rss);
			if (rss > g_rss_kb) {
				printf("[+] engine ready\n");
				return pid;
			}
		} else {
			printf("[*] t=%d no pid\n", i);
		}
		sleep(2);
	}
	fprintf(stderr, "[-] wait engine timed out\n");
	return 0;
}

static void kill_old_hud(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	if (!d)
		return;
	while ((e = readdir(d))) {
		char commp[64], comm[32];
		int fd, n;
		char *end = NULL;
		pid_t pid;
		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		pid = (pid_t)strtol(e->d_name, &end, 10);
		if (!end || *end || pid <= 1 || pid == getpid())
			continue;
		snprintf(commp, sizeof(commp), "/proc/%d/comm", (int)pid);
		fd = open(commp, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		n = (int)read(fd, comm, sizeof(comm) - 1);
		close(fd);
		if (n <= 0)
			continue;
		comm[n] = 0;
		if (comm[n - 1] == '\n')
			comm[--n] = 0;
		if (strcmp(comm, "update_verifier") == 0)
			kill(pid, SIGKILL);
	}
	closedir(d);
}

static int start_hud(const char *hud_path)
{
	pid_t p;
	int i;
	kill_old_hud();
	usleep(200 * 1000);
	p = fork();
	if (p < 0)
		return -1;
	if (p == 0) {
		int fd;
		char *av[] = {"update_verifier", NULL};
		setsid();
		fd = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
		prctl(PR_SET_NAME, "update_verifier", 0, 0, 0);
		execv(hud_path, av);
		_exit(127);
	}
	for (i = 0; i < 20; i++) {
		usleep(100 * 1000);
		if (kill(p, 0) == 0) {
			printf("[+] imgui up pid=%d\n", (int)p);
			return 0;
		}
	}
	fprintf(stderr, "[-] hud failed\n");
	return -1;
}

static int call_inject(const pack_paths *p, pid_t gpid)
{
	char delay[16], timeout[16], pidbuf[16];
	snprintf(delay, sizeof(delay), "%d", g_delay_ms);
	snprintf(timeout, sizeof(timeout), "%d", g_timeout);
	snprintf(pidbuf, sizeof(pidbuf), "%d", (int)gpid);
	char *av[] = {
		"i",
		"-p",
		(char *)g_pkg,
		"-s",
		(char *)p->so,
		"--pid",
		pidbuf,
		"--wait-lib",
		"",
		"--no-tinject",
		"--delay",
		delay,
		"--timeout",
		timeout,
		"--kpm",
		(char *)p->stealth,
		"--kpatch",
		(char *)p->kpatch,
		"--key",
		(char *)g_key,
		NULL,
	};
	int ac = 0;
	while (av[ac])
		ac++;
	return stealth_inject_main(ac, av);
}

static void scrub_code_cache(void)
{
	char dir[192];
	DIR *d;
	struct dirent *e;
	snprintf(dir, sizeof(dir), "/data/data/%s/code_cache", g_pkg);
	d = opendir(dir);
	if (!d)
		return;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		int hex = n == 8;
		char path[256];
		if (!hex)
			continue;
		for (size_t i = 0; i < n; i++) {
			char c = e->d_name[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
				hex = 0;
				break;
			}
		}
		if (!hex)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		unlink(path);
	}
	closedir(d);
}

int main(int argc, char **argv)
{
	pid_t gpid;
	int rc;

	memset(&g_pack, 0, sizeof(g_pack));
	atexit(wipe_now);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, on_sig);

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--restart"))
			g_restart = 1;
		else if (!strcmp(argv[i], "--no-hud"))
			g_no_hud = 1;
		else if (!strcmp(argv[i], "--key") && i + 1 < argc)
			g_key = argv[++i];
		else if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--package")) &&
			 i + 1 < argc)
			g_pkg = argv[++i];
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (geteuid() != 0) {
		fprintf(stderr, "[-] need root: su -c %s\n", argv[0]);
		return 1;
	}

	setenv("PATH",
	       "/system/bin:/system/xbin:/vendor/bin:/debug_ramdisk:/sbin:/data/local/tmp",
	       1);

	if (!g_key || !g_key[0]) {
		if (prompt_key(g_keybuf, sizeof(g_keybuf)) != 0) {
			fprintf(stderr, "[-] need APatch key (--key or prompt)\n");
			return 1;
		}
		g_key = g_keybuf;
	}

	if (pack_extract(&g_pack) != 0) {
		fprintf(stderr, "[-] extract failed\n");
		return 2;
	}
	printf("[+] extract %s\n", g_pack.dir);

	if (ensure_kpm(&g_pack) != 0) {
		wipe_now();
		return 3;
	}

	start_game(g_restart);
	gpid = wait_engine();
	if (gpid <= 0) {
		wipe_now();
		return 4;
	}

	rc = call_inject(&g_pack, gpid);
	if (rc != 0) {
		fprintf(stderr, "[-] inject failed\n");
		wipe_now();
		return 5;
	}
	printf("[+] inject done\n");
	scrub_code_cache();

	if (!g_no_hud) {
		if (start_hud(g_pack.hud) != 0) {
			wipe_now();
			return 6;
		}
	}
	wipe_now();
	printf("[+] wiped\n");
	return 0;
}
