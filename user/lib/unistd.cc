#include <unistd.h>
#include "syscall.h"

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
    return sys_ioperm(from, num, turn_on);
}

int ftruncate(int fd, off_t length) {
    return sys_ftruncate(fd, length);
}
