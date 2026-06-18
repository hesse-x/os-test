#include <sys/shm.h>
#include <errno.h>
#include "common/syscall.h"
#include "common/shm.h"

int shm_create(size_t size, void **addr) {
    void *r = sys_shm_create(size);
    if (r == NULL) {
        errno = ENOMEM;
        return -1;
    }
    *addr = r;
    return 0;
}

int shm_attach(pid_t target, void **addr) {
    void *r = sys_shm_attach(target, 0);
    if (r == NULL) {
        errno = ESRCH;
        return -1;
    }
    *addr = r;
    return 0;
}

int shm_attach_kernel(int shm_id, void **addr) {
    void *r = sys_shm_attach(shm_id, 1);
    if (r == NULL) {
        errno = ESRCH;
        return -1;
    }
    *addr = r;
    return 0;
}
