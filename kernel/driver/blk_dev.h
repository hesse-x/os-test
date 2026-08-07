/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BLK_DEV_H
#define KERNEL_BLK_DEV_H

#include "kernel/bsd/devtmpfs.h"
#include "kernel/xcore/atomic.h"
#include <stdbool.h>
#include <stdint.h>

#define BLOCK_MAX_PARTITIONS 4

struct block_device_ops {
  int (*read)(void *ctx, uint64_t sector, uint32_t count, void *buf);
  int (*write)(void *ctx, uint64_t sector, uint32_t count, const void *buf);
  int (*flush)(void *ctx);
};

struct block_partition {
  struct block_device *bdev;
  uint64_t start_sector;
  uint64_t sector_count;
  uint8_t mbr_type;
  uint8_t index;
  atomic_t openers;
  bool mounted;
};

struct block_device {
  const struct block_device_ops *ops;
  void *ctx;
  uint64_t sectors;
  uint32_t logical_sector_size;
  uint32_t max_sectors_per_io;
  const char *name;
  struct block_partition partitions[BLOCK_MAX_PARTITIONS];
  uint8_t partition_count;
  bool partitions_scanned;
  int partition_scan_result;
};

struct blk_stats {
  uint64_t submitted;
  uint64_t completed;
  uint64_t failed;
  uint64_t validation_rejected;
  uint64_t read_cmds;
  uint64_t write_cmds;
  uint64_t flush_cmds;
  uint64_t read_sectors;
  uint64_t write_sectors;
  uint64_t read_size_buckets[5];
  uint64_t write_size_buckets[5];
};

int block_read(struct block_device *bdev, uint64_t sector, uint32_t count,
               void *buf);
int block_write(struct block_device *bdev, uint64_t sector, uint32_t count,
                const void *buf);
int block_flush(struct block_device *bdev);
int partition_read(struct block_partition *part, uint64_t sector,
                   uint32_t count, void *buf);
int partition_write(struct block_partition *part, uint64_t sector,
                    uint32_t count, const void *buf);
int partition_flush(struct block_partition *part);
int block_scan_mbr(struct block_device *bdev);
int block_init_ahci(void);
int block_publish_devtmpfs(void);
struct block_device *block_primary_device(void);
struct block_partition *block_partition_get(struct block_device *bdev,
                                            uint8_t index);

int blk_read(uint32_t lba, uint32_t count, void *buf);
int blk_write(uint32_t lba, uint32_t count, const void *buf);
int blk_read_sector(uint32_t lba, void *buf);
int blk_flush(void);
void blk_get_stats(struct blk_stats *out);

extern struct dev_ops blk_dev_ops;

#endif
