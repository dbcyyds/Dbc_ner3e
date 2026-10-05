#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

extern char **environ;

int main(int argc, char **argv)
{
    /* libc execve() stats the path first. kpcloak makes that stat return
     * ENOENT. The kernel still redirects the raw execve for an allowlisted uid. */
    unsetenv("LD_LIBRARY_PATH");
    unsetenv("LD_PRELOAD");
    argv[0] = "/system/bin/su";
    syscall(__NR_execve, "/system/bin/su", argv, environ);
    return 127;
}
