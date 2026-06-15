#include <sys/mman.h>
#include <errno.h>
#include "common/syscall.h"

void *mmap(void *addr, size_t length, int prot, int flags, uint64_t offset) {
    void *r = sys_mmap(addr, length, prot, flags, offset);
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
