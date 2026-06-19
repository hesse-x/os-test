#include <sys/block.h>
#include <errno.h>
#include "common/syscall.h"

int block_read(uint32_t lba, void *buf, uint32_t count) {
    int r = sys_block_io(lba, buf, count, BLOCK_DIR_READ);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int block_write(uint32_t lba, const void *data, uint32_t count) {
    int r = sys_block_io(lba, (void *)data, count, BLOCK_DIR_WRITE);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int block_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir) {
    int r = sys_block_async(lba, buf, count, dir);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}
