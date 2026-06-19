#include <sys/mman.h>
#include <sys/shm.h>
#include <errno.h>
#include <unistd.h>
#include "common/syscall.h"

// Defined in file.cc: returns target_pid for FD_DEV fd, or -1 if not FD_DEV
pid_t __fd_dev_target_pid(int fd);

// POSIX-like mmap: (addr, length, prot, flags, fd, offset)
// For MAP_SHARED (SHM): fd is the SHM fd or FD_DEV fd
// For MAP_PHYSICAL: fd=-1, offset is phys addr
// For anonymous: fd=-1, offset ignored
void *mmap(void *addr, size_t length, int prot, int flags, int fd, uint64_t offset) {
    // FD_DEV mmap via MAP_SHARED: transitional path
    // Opens the driver's SHM fd and maps it
    if (flags & MAP_SHARED) {
        pid_t target_pid = __fd_dev_target_pid(fd);
        if (target_pid > 0) {
            // Transitional: attach to driver's SHM, get an fd, then mmap it
            int shm_fd = sys_shm_attach(target_pid, 0);
            if (shm_fd <= 0) {
                errno = EBADF;
                return MAP_FAILED;
            }
            void *ptr = sys_mmap(NULL, length, prot, flags, shm_fd, 0);
            sys_close(shm_fd);  // mmap keeps a reference
            if (!ptr) {
                errno = ENOMEM;
                return MAP_FAILED;
            }
            return ptr;
        }
        // Direct SHM fd mmap (fd already is an SHM fd)
        void *r = sys_mmap(addr, length, prot, flags, fd, offset);
        if (!r) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
        return r;
    }

    // MAP_PHYSICAL: pass fd=-1, offset=phys_addr
    // Anonymous: pass fd=-1, offset=0
    void *r = sys_mmap(addr, length, prot, flags, -1, offset);
    if (!r) {
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
