#include <sys/device.h>
#include <errno.h>
#include "common/syscall.h"

int device_register(pid_t pid, int dev_type) {
    int r = sys_load_dev(pid, dev_type);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

pid_t device_lookup(int dev_type) {
    int32_t r = sys_lookup_dev(dev_type);
    if (r <= 0) {
        if (r < 0) errno = -r;
        else errno = ESRCH;
        return -1;
    }
    return (pid_t)r;
}
