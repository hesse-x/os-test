#include <sys/device.h>
#include <errno.h>
#include <string.h>
#include "common/syscall.h"
#include "common/dev.h"

int device_register(const char *name, int dev_type) {
    return device_register_shm(name, dev_type, -1);
}

int device_register_shm(const char *name, int dev_type, int shm_fd) {
    // Create devtmpfs node so open("/dev/<name>") works
    // Kernel auto-fills driver_pid=current_task->pid, callbacks=NULL
    // shm_fd: FD_SHM fd to bind to device inode (-1 = no SHM)
    int r = sys_dev_create(name, dev_type, shm_fd);
    // sys_dev_create returns -1 on error with errno set.
    // EEXIST means device already exists — treat as success.
    if (r < 0 && errno != EEXIST) return -1;
    return 0;
}
