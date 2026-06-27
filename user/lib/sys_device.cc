#include <sys/device.h>
#include <errno.h>
#include <string.h>
#include "common/syscall.h"
#include "common/dev.h"

int device_register(const char *name, int dev_type) {
    // Create devtmpfs node so open("/dev/<name>") works
    // Kernel auto-fills driver_pid=current_task->pid, callbacks=NULL
    int r = sys_dev_create(name, dev_type);
    if (r < 0 && r != -EEXIST) {
        errno = -r;
        return -1;
    }
    return 0;
}
