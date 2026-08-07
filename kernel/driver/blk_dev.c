/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/ahci.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/driver/bsd_types.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/xtask.h"
#include <xos/errno.h>

enum block_node_kind { BLOCK_NODE_DISK, BLOCK_NODE_PARTITION };

struct block_node {
  enum block_node_kind kind;
  union {
    struct block_device *bdev;
    struct block_partition *part;
  } object;
};

static struct blk_stats stats;
static struct block_device ahci_bdev;
static struct block_node ahci_disk_node;
static struct block_node ahci_partition_nodes[BLOCK_MAX_PARTITIONS];

static bool is_power_of_two(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static int block_validate_range(struct block_device *bdev, uint64_t sector,
                                uint32_t count, const void *buf) {
  if (!bdev || !bdev->ops || !buf)
    return !buf ? -EFAULT : -EINVAL;
  if (!count || !bdev->sectors || !is_power_of_two(bdev->logical_sector_size) ||
      !bdev->max_sectors_per_io)
    return -EINVAL;
  if (sector >= bdev->sectors || (uint64_t)count > bdev->sectors - sector)
    return -EINVAL;
  return 0;
}

static unsigned blk_size_bucket(uint32_t count, uint32_t max_sectors) {
  if (count == 1)
    return 0;
  if (count < 8)
    return 1;
  if (count == 8)
    return 2;
  if (count < max_sectors)
    return 3;
  return 4;
}

static void block_account_start(bool write, uint32_t count,
                                uint32_t max_sectors) {
  __atomic_fetch_add(&stats.submitted, 1, __ATOMIC_RELAXED);
  if (write) {
    __atomic_fetch_add(&stats.write_cmds, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&stats.write_sectors, count, __ATOMIC_RELAXED);
    __atomic_fetch_add(
        &stats.write_size_buckets[blk_size_bucket(count, max_sectors)], 1,
        __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&stats.read_cmds, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&stats.read_sectors, count, __ATOMIC_RELAXED);
    __atomic_fetch_add(
        &stats.read_size_buckets[blk_size_bucket(count, max_sectors)], 1,
        __ATOMIC_RELAXED);
  }
}

static void block_account_done(int rc) {
  __atomic_fetch_add(rc ? &stats.failed : &stats.completed, 1,
                     __ATOMIC_RELAXED);
}

int block_read(struct block_device *bdev, uint64_t sector, uint32_t count,
               void *buf) {
  int rc = block_validate_range(bdev, sector, count, buf);
  if (rc) {
    __atomic_fetch_add(&stats.validation_rejected, 1, __ATOMIC_RELAXED);
    return rc;
  }
  block_account_start(false, count, bdev->max_sectors_per_io);
  uint32_t remaining = count;
  uint8_t *cursor = buf;
  while (remaining) {
    uint32_t chunk = remaining > bdev->max_sectors_per_io
                         ? bdev->max_sectors_per_io
                         : remaining;
    rc = bdev->ops->read(bdev->ctx, sector, chunk, cursor);
    if (rc)
      break;
    sector += chunk;
    remaining -= chunk;
    cursor += (size_t)chunk * bdev->logical_sector_size;
  }
  block_account_done(rc);
  return rc;
}

int block_write(struct block_device *bdev, uint64_t sector, uint32_t count,
                const void *buf) {
  int rc = block_validate_range(bdev, sector, count, buf);
  if (rc) {
    __atomic_fetch_add(&stats.validation_rejected, 1, __ATOMIC_RELAXED);
    return rc;
  }
  if (!bdev->ops->write)
    return -EOPNOTSUPP;
  block_account_start(true, count, bdev->max_sectors_per_io);
  uint32_t remaining = count;
  const uint8_t *cursor = buf;
  while (remaining) {
    uint32_t chunk = remaining > bdev->max_sectors_per_io
                         ? bdev->max_sectors_per_io
                         : remaining;
    rc = bdev->ops->write(bdev->ctx, sector, chunk, cursor);
    if (rc)
      break;
    sector += chunk;
    remaining -= chunk;
    cursor += (size_t)chunk * bdev->logical_sector_size;
  }
  block_account_done(rc);
  return rc;
}

int block_flush(struct block_device *bdev) {
  if (!bdev || !bdev->ops)
    return -EINVAL;
  if (!bdev->ops->flush)
    return -EOPNOTSUPP;
  __atomic_fetch_add(&stats.submitted, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&stats.flush_cmds, 1, __ATOMIC_RELAXED);
  int rc = bdev->ops->flush(bdev->ctx);
  block_account_done(rc);
  return rc;
}

static int partition_translate(struct block_partition *part, uint64_t sector,
                               uint32_t count, uint64_t *absolute) {
  if (!part || !part->bdev || !count || sector >= part->sector_count ||
      (uint64_t)count > part->sector_count - sector)
    return -EINVAL;
  if (part->start_sector > UINT64_MAX - sector)
    return -EINVAL;
  *absolute = part->start_sector + sector;
  return 0;
}

int partition_read(struct block_partition *part, uint64_t sector,
                   uint32_t count, void *buf) {
  if (!buf)
    return -EFAULT;
  uint64_t absolute;
  int rc = partition_translate(part, sector, count, &absolute);
  if (rc) {
    __atomic_fetch_add(&stats.validation_rejected, 1, __ATOMIC_RELAXED);
    return rc;
  }
  return block_read(part->bdev, absolute, count, buf);
}

int partition_write(struct block_partition *part, uint64_t sector,
                    uint32_t count, const void *buf) {
  if (!buf)
    return -EFAULT;
  uint64_t absolute;
  int rc = partition_translate(part, sector, count, &absolute);
  if (rc) {
    __atomic_fetch_add(&stats.validation_rejected, 1, __ATOMIC_RELAXED);
    return rc;
  }
  return block_write(part->bdev, absolute, count, buf);
}

int partition_flush(struct block_partition *part) {
  return part ? block_flush(part->bdev) : -EINVAL;
}

static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static bool unsupported_partition_type(uint8_t type) {
  return type == 0x05 || type == 0x0f || type == 0x85 || type == 0xee;
}

int block_scan_mbr(struct block_device *bdev) {
  if (!bdev)
    return -EINVAL;
  if (bdev->partitions_scanned)
    return bdev->partition_scan_result;

  int rc = 0;
  uint8_t sector[4096];
  struct block_partition parsed[BLOCK_MAX_PARTITIONS] = {0};
  uint8_t found = 0;
  if (bdev->logical_sector_size < 512 ||
      bdev->logical_sector_size > sizeof(sector)) {
    rc = -EINVAL;
    goto done;
  }
  rc = block_read(bdev, 0, 1, sector);
  if (rc)
    goto done;
  if (sector[510] != 0x55 || sector[511] != 0xaa) {
    rc = -EINVAL;
    goto done;
  }

  for (uint8_t i = 0; i < BLOCK_MAX_PARTITIONS; i++) {
    const uint8_t *entry = sector + 0x1be + (size_t)i * 16;
    uint8_t type = entry[4];
    uint64_t start = get_le32(entry + 8);
    uint64_t count = get_le32(entry + 12);
    if (!type && !start && !count)
      continue;
    if (!type || !start || !count || unsupported_partition_type(type) ||
        start >= bdev->sectors || count > bdev->sectors - start) {
      rc = -EINVAL;
      goto done;
    }
    uint64_t end = start + count;
    for (uint8_t j = 0; j < BLOCK_MAX_PARTITIONS; j++) {
      if (!parsed[j].bdev)
        continue;
      uint64_t other_end = parsed[j].start_sector + parsed[j].sector_count;
      if (start < other_end && parsed[j].start_sector < end) {
        rc = -EINVAL;
        goto done;
      }
    }
    parsed[i].bdev = bdev;
    parsed[i].start_sector = start;
    parsed[i].sector_count = count;
    parsed[i].mbr_type = type;
    parsed[i].index = i + 1;
    atomic_set(&parsed[i].openers, 0);
    found++;
  }
  if (!found) {
    rc = -EINVAL;
    goto done;
  }
  for (uint8_t i = 0; i < BLOCK_MAX_PARTITIONS; i++)
    bdev->partitions[i] = parsed[i];
  bdev->partition_count = found;

done:
  bdev->partition_scan_result = rc;
  bdev->partitions_scanned = true;
  return rc;
}

static int ahci_block_read(void *ctx, uint64_t sector, uint32_t count,
                           void *buf) {
  (void)ctx;
  if (sector > UINT32_MAX || (uint64_t)count - 1 > UINT32_MAX - sector)
    return -EINVAL;
  return ahci_submit_sync((uint32_t)sector, count, buf, 0);
}

static int ahci_block_write(void *ctx, uint64_t sector, uint32_t count,
                            const void *buf) {
  (void)ctx;
  if (sector > UINT32_MAX || (uint64_t)count - 1 > UINT32_MAX - sector)
    return -EINVAL;
  return ahci_submit_sync((uint32_t)sector, count, (void *)buf, 1);
}

static int ahci_block_flush(void *ctx) {
  (void)ctx;
  return ahci_flush_cache();
}

static const struct block_device_ops ahci_block_ops = {
    .read = ahci_block_read,
    .write = ahci_block_write,
    .flush = ahci_block_flush,
};

int block_init_ahci(void) {
  if (ahci_bdev.ops)
    return ahci_bdev.partition_scan_result;
  ahci_bdev.ops = &ahci_block_ops;
  ahci_bdev.ctx = NULL;
  ahci_bdev.sectors = ahci_sector_count();
  ahci_bdev.logical_sector_size = 512;
  ahci_bdev.max_sectors_per_io = AHCI_MAX_SECTORS;
  ahci_bdev.name = "sda";
  if (!ahci_bdev.sectors)
    return -ENODEV;
  int rc = block_scan_mbr(&ahci_bdev);
  printk(rc ? LOG_ERROR : LOG_INFO,
         "block: %s sectors=%lu sector_size=%u max_io=%u scan=%d\n",
         ahci_bdev.name, ahci_bdev.sectors, ahci_bdev.logical_sector_size,
         ahci_bdev.max_sectors_per_io, rc);
  if (!rc) {
    for (uint8_t i = 0; i < BLOCK_MAX_PARTITIONS; i++) {
      struct block_partition *part = &ahci_bdev.partitions[i];
      if (part->bdev)
        printk(LOG_INFO, "block: %s%u type=%x start=%lu sectors=%lu\n",
               ahci_bdev.name, part->index, part->mbr_type, part->start_sector,
               part->sector_count);
    }
  }
  return rc;
}

struct block_device *block_primary_device(void) {
  return ahci_bdev.ops ? &ahci_bdev : NULL;
}

struct block_partition *block_partition_get(struct block_device *bdev,
                                            uint8_t index) {
  if (!bdev || index < 1 || index > BLOCK_MAX_PARTITIONS)
    return NULL;
  struct block_partition *part = &bdev->partitions[index - 1];
  return part->bdev ? part : NULL;
}

int block_publish_devtmpfs(void) {
  struct block_device *bdev = block_primary_device();
  if (!bdev || bdev->partition_scan_result)
    return -ENODEV;
  ahci_disk_node.kind = BLOCK_NODE_DISK;
  ahci_disk_node.object.bdev = bdev;
  int rc =
      devtmpfs_create_device(bdev->name, &blk_dev_ops, &ahci_disk_node, NULL);
  if (rc && rc != -EEXIST)
    return rc;
  for (uint8_t i = 0; i < BLOCK_MAX_PARTITIONS; i++) {
    struct block_partition *part = &bdev->partitions[i];
    if (!part->bdev)
      continue;
    char name[8] = {'s', 'd', 'a', (char)('0' + part->index), 0};
    ahci_partition_nodes[i].kind = BLOCK_NODE_PARTITION;
    ahci_partition_nodes[i].object.part = part;
    rc = devtmpfs_create_device(name, &blk_dev_ops, &ahci_partition_nodes[i],
                                NULL);
    if (rc && rc != -EEXIST)
      return rc;
  }
  return 0;
}

/* Compatibility wrappers are kept for non-filesystem diagnostics only. */
int blk_read(uint32_t lba, uint32_t count, void *buf) {
  return block_read(block_primary_device(), lba, count, buf);
}

int blk_write(uint32_t lba, uint32_t count, const void *buf) {
  return block_write(block_primary_device(), lba, count, buf);
}

int blk_read_sector(uint32_t lba, void *buf) { return blk_read(lba, 1, buf); }

int blk_flush(void) { return block_flush(block_primary_device()); }

void blk_get_stats(struct blk_stats *out) {
  if (!out)
    return;
  const uint64_t *src = (const uint64_t *)&stats;
  uint64_t *dst = (uint64_t *)out;
  for (size_t i = 0; i < sizeof(stats) / sizeof(uint64_t); i++)
    dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
}

static struct block_node *block_file_node(struct file *file) {
  return file ? devtmpfs_device_private(file->inode) : NULL;
}

static uint64_t block_node_sectors(const struct block_node *node) {
  return node->kind == BLOCK_NODE_PARTITION ? node->object.part->sector_count
                                            : node->object.bdev->sectors;
}

static uint32_t block_node_sector_size(const struct block_node *node) {
  return node->kind == BLOCK_NODE_PARTITION
             ? node->object.part->bdev->logical_sector_size
             : node->object.bdev->logical_sector_size;
}

static int block_node_read(const struct block_node *node, uint64_t sector,
                           uint32_t count, void *buf) {
  return node->kind == BLOCK_NODE_PARTITION
             ? partition_read(node->object.part, sector, count, buf)
             : block_read(node->object.bdev, sector, count, buf);
}

static int block_node_write(const struct block_node *node, uint64_t sector,
                            uint32_t count, const void *buf) {
  return node->kind == BLOCK_NODE_PARTITION
             ? partition_write(node->object.part, sector, count, buf)
             : block_write(node->object.bdev, sector, count, buf);
}

static int blk_dev_open(xtask *proc, int fd) {
  struct file *file = fd_lookup(proc->proc->files, fd);
  struct block_node *node = block_file_node(file);
  if (!node)
    return -ENODEV;
  if (node->kind == BLOCK_NODE_PARTITION)
    atomic_inc(&node->object.part->openers);
  return 0;
}

static int blk_dev_close(xtask *proc, int fd) {
  struct file *file = fd_lookup(proc->proc->files, fd);
  struct block_node *node = block_file_node(file);
  if (node && node->kind == BLOCK_NODE_PARTITION)
    atomic_dec(&node->object.part->openers);
  return 0;
}

static ssize_t blk_dev_read(xtask *proc, int fd, void *buf, size_t count) {
  struct file *file = fd_lookup(proc->proc->files, fd);
  if (!file)
    return -EBADF;
  struct block_node *node = block_file_node(file);
  if (!node)
    return -ENODEV;
  uint32_t sector_size = block_node_sector_size(node);
  uint64_t capacity = block_node_sectors(node);
  if (file->offset % sector_size || count % sector_size)
    return -EINVAL;
  uint64_t sector = file->offset / sector_size;
  if (sector == capacity)
    return 0;
  uint64_t sectors64 = count / sector_size;
  if (!sectors64 || sectors64 > UINT32_MAX || sector > capacity ||
      sectors64 > capacity - sector)
    return -EINVAL;
  int rc = block_node_read(node, sector, (uint32_t)sectors64, buf);
  if (rc)
    return rc;
  file->offset += count;
  return (ssize_t)count;
}

static ssize_t blk_dev_write(xtask *proc, int fd, const void *buf,
                             size_t count) {
  struct file *file = fd_lookup(proc->proc->files, fd);
  if (!file)
    return -EBADF;
  struct block_node *node = block_file_node(file);
  if (!node)
    return -ENODEV;
  uint32_t sector_size = block_node_sector_size(node);
  uint64_t capacity = block_node_sectors(node);
  if (file->offset % sector_size || count % sector_size)
    return -EINVAL;
  uint64_t sector = file->offset / sector_size;
  uint64_t sectors64 = count / sector_size;
  if (!sectors64 || sectors64 > UINT32_MAX || sector >= capacity ||
      sectors64 > capacity - sector)
    return -EINVAL;
  int rc = block_node_write(node, sector, (uint32_t)sectors64, buf);
  if (rc)
    return rc;
  file->offset += count;
  return (ssize_t)count;
}

struct dev_ops blk_dev_ops = {
    .driver_pid = 0,
    .is_block = true,
    .open = blk_dev_open,
    .close = blk_dev_close,
    .read = blk_dev_read,
    .write = blk_dev_write,
};
