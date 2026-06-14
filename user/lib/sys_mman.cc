#include <sys/mman.h>
#include <errno.h>
#include "common/syscall.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
    void *r = sys_mmap(length);
    if (r == NULL) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return r;
}

int munmap(void *addr, size_t length) {
    int r = sys_munmap(addr, length);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}
