#include <sys/wait.h>
#include <stdint.h>
#include "syscall.h"

pid_t waitpid(pid_t pid, int *status, int options) {
    (void)options;
    int64_t r = sys_waitpid(pid, status);
    if (r < 0) return -1;
    return (pid_t)r;
}
