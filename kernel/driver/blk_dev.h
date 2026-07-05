#ifndef KERNEL_BLK_DEV_H
#define KERNEL_BLK_DEV_H

#include <stdint.h>
#include "kernel/bsd/devtmpfs.h"

int blk_read(uint32_t lba, uint32_t count, void *buf);
int blk_write(uint32_t lba, uint32_t count, const void *buf);
int blk_read_sector(uint32_t lba, void *buf);

struct dev_ops;
extern struct dev_ops blk_dev_ops;

#endif
