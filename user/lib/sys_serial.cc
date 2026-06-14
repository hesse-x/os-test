#include <sys/serial.h>
#include <errno.h>
#include "common/syscall.h"

int serial_write(const char *buf, size_t len) {
    int r = sys_serial_write(buf, len);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}
