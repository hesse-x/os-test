#include <unistd.h>
#include <errno.h>
#include "common/syscall.h"

int errno;

pid_t getpid(void) {
    return (pid_t)sys_getpid();
}

pid_t gettid(void) {
    return (pid_t)sys_gettid();
}

void _exit(int status) {
    sys_exit(status);
    __builtin_unreachable();
}

int sched_yield(void) {
    sys_yield();
    return 0;
}

int ioperm(unsigned long from, unsigned long num, int turn_on) {
    int r = sys_ioperm(from, num, turn_on);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int ftruncate(int fd, off_t length) {
    int r = sys_ftruncate(fd, length);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}
