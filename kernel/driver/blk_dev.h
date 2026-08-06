/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BLK_DEV_H
#define KERNEL_BLK_DEV_H

#include "kernel/bsd/devtmpfs.h"
#include <stdint.h>

struct blk_stats {
  uint64_t submitted;
  uint64_t completed;
  uint64_t failed;
  uint64_t read_cmds;
  uint64_t write_cmds;
  uint64_t read_sectors;
  uint64_t write_sectors;
};

int blk_read(uint32_t lba, uint32_t count, void *buf);
int blk_write(uint32_t lba, uint32_t count, const void *buf);
int blk_read_sector(uint32_t lba, void *buf);
void blk_get_stats(struct blk_stats *out);

extern struct dev_ops blk_dev_ops;

#endif
