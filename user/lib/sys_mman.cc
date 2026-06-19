#include <sys/mman.h>
#include <sys/shm.h>
#include <errno.h>
#include <unistd.h>
#include "common/syscall.h"

// Defined in file.cc: returns target_pid for FD_DEV fd, or -1 if not FD_DEV
pid_t __fd_dev_target_pid(int fd);

void *mmap(void *addr, size_t length, int prot, int flags, uint64_t fd_val) {
    // FD_DEV mmap via MAP_SHARED: map driver SHM through fd
    if (flags & MAP_SHARED) {
        int fd = (int)fd_val;
        pid_t target_pid = __fd_dev_target_pid(fd);
        if (target_pid > 0) {
            void *ptr = (void *)sys_shm_attach(target_pid, 0);
            if (ptr) return ptr;
        }
        errno = EBADF;
        return MAP_FAILED;
    }

    // Anonymous or MAP_PHYSICAL
    void *r = sys_mmap(addr, length, prot, flags, fd_val);
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
