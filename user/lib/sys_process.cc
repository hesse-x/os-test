#include <sys/process.h>
#include <errno.h>
#include <unistd.h>
#include "common/syscall.h"

pid_t fork(void) {
    int64_t r = sys_fork();
    if (r < 0) { errno = (int)(-r); return -1; }
    return (pid_t)r;
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    int r = sys_execve(pathname, argv, envp);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

pid_t spawn(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        execve(path, NULL, NULL);
        _exit(127);
        __builtin_unreachable();
    }
    return pid;
}
