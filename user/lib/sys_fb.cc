#include <errno.h>
#include "common/syscall.h"

// Deprecated: sys_fb_info now returns -ENOSYS
int fb_info(void *buf) {
    (void)buf;
    errno = ENOSYS;
    return -1;
}
