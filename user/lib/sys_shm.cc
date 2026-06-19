#include <sys/shm.h>
#include <sys/mman.h>
#include <errno.h>
#include "common/syscall.h"
#include "common/shm.h"

int shm_create(size_t size, void **addr) {
    // Create SHM: returns fd
    int fd = sys_shm_create(size);
    if (fd <= 0) {
        errno = ENOMEM;
        return -1;
    }
    // Map the SHM fd to get vaddr
    void *vaddr = sys_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (!vaddr) {
        sys_close(fd);
        errno = ENOMEM;
        return -1;
    }
    *addr = vaddr;
    // Keep fd open — sys_shm_attach(mode=0) scans the creator's fd_table for FD_SHM
    // to locate this SHM. proc_reap will close it on process exit.
    return 0;
}

int shm_attach(pid_t target, void **addr) {
    int fd = sys_shm_attach(target, 0);
    if (fd <= 0) {
        errno = ESRCH;
        return -1;
    }
    void *vaddr = sys_mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (!vaddr) {
        sys_close(fd);
        errno = ENOMEM;
        return -1;
    }
    *addr = vaddr;
    sys_close(fd);  // optional — mmap keeps reference
    return 0;
}

int shm_attach_kernel(int shm_id, void **addr) {
    int fd = sys_shm_attach(shm_id, 1);
    if (fd <= 0) {
        errno = ESRCH;
        return -1;
    }
    void *vaddr = sys_mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (!vaddr) {
        sys_close(fd);
        errno = ENOMEM;
        return -1;
    }
    *addr = vaddr;
    sys_close(fd);  // optional — mmap keeps reference
    return 0;
}
