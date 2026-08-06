/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>

#include "kernel/driver/ahci.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/spinlock.h"
#include <xos/errno.h>

static struct blk_stats stats;

static int blk_validate(uint32_t lba, uint32_t count, const void *buf) {
  if (!buf)
    return -EFAULT;
  if (count == 0 || count > AHCI_MAX_SECTORS)
    return -EINVAL;
  uint64_t capacity = ahci_sector_count();
  if (capacity && (uint64_t)lba + count > capacity)
    return -EINVAL;
  return 0;
}

int blk_read(uint32_t lba, uint32_t count, void *buf) {
  int valid = blk_validate(lba, count, buf);
  if (valid)
    return valid;
  __atomic_fetch_add(&stats.submitted, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&stats.read_cmds, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&stats.read_sectors, count, __ATOMIC_RELAXED);
  uint64_t flags;
  spin_lock_irqsave(&ahci_lock, &flags);
  int rc = ahci_read_lba(lba, count, buf);
  spin_unlock_irqrestore(&ahci_lock, flags);
  __atomic_fetch_add(rc ? &stats.failed : &stats.completed, 1,
                     __ATOMIC_RELAXED);
  return rc;
}

int blk_write(uint32_t lba, uint32_t count, const void *buf) {
  int valid = blk_validate(lba, count, buf);
  if (valid)
    return valid;
  __atomic_fetch_add(&stats.submitted, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&stats.write_cmds, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&stats.write_sectors, count, __ATOMIC_RELAXED);
  uint64_t flags;
  spin_lock_irqsave(&ahci_lock, &flags);
  int rc = ahci_write_lba(lba, count, buf);
  spin_unlock_irqrestore(&ahci_lock, flags);
  __atomic_fetch_add(rc ? &stats.failed : &stats.completed, 1,
                     __ATOMIC_RELAXED);
  return rc;
}

int blk_read_sector(uint32_t lba, void *buf) { return blk_read(lba, 1, buf); }

void blk_get_stats(struct blk_stats *out) {
  if (!out)
    return;
  out->submitted = __atomic_load_n(&stats.submitted, __ATOMIC_RELAXED);
  out->completed = __atomic_load_n(&stats.completed, __ATOMIC_RELAXED);
  out->failed = __atomic_load_n(&stats.failed, __ATOMIC_RELAXED);
  out->read_cmds = __atomic_load_n(&stats.read_cmds, __ATOMIC_RELAXED);
  out->write_cmds = __atomic_load_n(&stats.write_cmds, __ATOMIC_RELAXED);
  out->read_sectors = __atomic_load_n(&stats.read_sectors, __ATOMIC_RELAXED);
  out->write_sectors = __atomic_load_n(&stats.write_sectors, __ATOMIC_RELAXED);
}

#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/bsd_types.h"
#include "kernel/xcore/xtask.h"

static int blk_dev_open(xtask *proc, int fd) { return 0; }

static int blk_dev_close(xtask *proc, int fd) { return 0; }

static ssize_t blk_dev_read(xtask *proc, int fd, void *buf, size_t count) {
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (WARN_ON_ONCE(!f))
    return -EBADF;
  uint64_t off = f->offset;

  if (off % 512 != 0 || count % 512 != 0)
    return -EINVAL;

  uint32_t lba = (uint32_t)(off / 512);
  uint32_t nsec = (uint32_t)(count / 512);

  if (nsec > AHCI_MAX_SECTORS)
    nsec = AHCI_MAX_SECTORS;

  int ret = blk_read(lba, nsec, buf);
  if (ret < 0)
    return ret;

  size_t done = nsec * 512;
  f->offset += done;
  return (ssize_t)done;
}

static ssize_t blk_dev_write(xtask *proc, int fd, const void *buf,
                             size_t count) {
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (WARN_ON_ONCE(!f))
    return -EBADF;
  uint64_t off = f->offset;

  if (off % 512 != 0 || count % 512 != 0)
    return -EINVAL;

  uint32_t lba = (uint32_t)(off / 512);
  uint32_t nsec = (uint32_t)(count / 512);

  if (nsec > AHCI_MAX_SECTORS)
    nsec = AHCI_MAX_SECTORS;

  int ret = blk_write(lba, nsec, buf);
  if (ret < 0)
    return ret;

  size_t done = nsec * 512;
  f->offset += done;
  return (ssize_t)done;
}

struct dev_ops blk_dev_ops = {
    .driver_pid = 0,
    .is_block = true,
    .open = blk_dev_open,
    .close = blk_dev_close,
    .read = blk_dev_read,
    .write = blk_dev_write,
    .ioctl = NULL,
    .mmap = NULL,
    .poll = NULL,
};
