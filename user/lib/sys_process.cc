#include <sys/process.h>
#include <errno.h>
#include "common/syscall.h"

pid_t spawn(const void *elf, size_t size) {
    int64_t r = sys_spawn(elf, size);
    if (r <= 0) {
        if (r < 0) errno = (int)(-r);
        else errno = ENOMEM;
        return -1;
    }
    return (pid_t)r;
}
