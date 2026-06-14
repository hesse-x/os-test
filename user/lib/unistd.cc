#include <unistd.h>
#include <errno.h>
#include "common/syscall.h"

int errno;

pid_t getpid(void) {
    return (pid_t)sys_getpid();
}

void _exit(int status) {
    sys_exit(status);
    __builtin_unreachable();
}

ssize_t read(int fd, void *buf, size_t count) {
    int64_t r = sys_read(fd, buf, count);
    if (r < 0) {
        errno = (int)(-r);
        return -1;
    }
    return (ssize_t)r;
}

ssize_t write(int fd, const void *buf, size_t count) {
    int64_t r = sys_write(fd, buf, count);
    if (r < 0) {
        errno = (int)(-r);
        return -1;
    }
    return (ssize_t)r;
}

int close(int fd) {
    int r = sys_close(fd);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int pipe(int fd[2]) {
    int r = sys_pipe(fd);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int sched_yield(void) {
    sys_yield();
    return 0;
}
