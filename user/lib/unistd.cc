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

int sched_yield(void) {
    sys_yield();
    return 0;
}
