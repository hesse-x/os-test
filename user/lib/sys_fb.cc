#include <sys/fb.h>
#include <errno.h>
#include "common/syscall.h"

int fb_info(void *buf) {
    int r = sys_fb_info(buf);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}
