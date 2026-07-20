/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_DEVTMPFS_H
#define KERNEL_DEVTMPFS_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/bsd/mount.h"
#include "kernel/bsd/poll_types.h" // __poll
#include "kernel/xcore/xtask.h"    // pid_t

/* Linux 64-bit dev_t 编码（搬自 user/include/sys/sysmacros.h，内核侧共用）。
 * 内核不依赖用户态 sysmacros.h，故在此独立定义；纯算术无外部依赖。 */
static inline uint64_t k_makedev(uint32_t major, uint32_t minor) {
  return ((uint64_t)(major & 0xfff) << 8) | ((uint64_t)(major & ~0xfff) << 32) |
         ((uint64_t)(minor & 0xff)) | ((uint64_t)(minor & ~0xff) << 12);
}

struct shm;

struct dev_ops {
  pid_t driver_pid; // 0 = kernel device, >0 = user-space driver
  bool is_block;    // true = block device, false = char device
  uint32_t minor;   // device minor number (ioctl req routing)

  char subsystem[8]; // "input" / "drm" / "block" / "tty"
  char devtype[8];   // "evdev" / "card" / "disk" / "ptmx"
  void *subsys_priv; // -> input_dev_props* / NULL
  void *uevent_priv; // -> uevent_attr_priv* (sysfs uevent attr 的 priv) / NULL
  struct sysfs_node *sysfs_dir; // sysfs 子树根 (移除时用)

  // VFS callbacks (only called when driver_pid == 0)
  int (*open)(xtask *proc, int fd);
  int (*close)(xtask *proc, int fd);
  long (*ioctl)(uint32_t cmd, void *arg);
  uint64_t (*mmap)(xtask *proc, uint64_t size, uint64_t offset);
  ssize_t (*read)(xtask *proc, int fd, void *buf, size_t count);
  ssize_t (*write)(xtask *proc, int fd, const void *buf, size_t count);
  __poll (*poll)(xtask *proc, int events);
};

void devtmpfs_init(void);
int devtmpfs_create(const char *name, struct dev_ops *ops, struct shm *shm);
uint64_t devtmpfs_open(xtask *proc, const char *name, int flags,
                       struct mount_entry *m);
struct inode *devtmpfs_lookup(const char *name);
void devtmpfs_cleanup_pid(pid_t pid);
void devtmpfs_remove(const char *name);

extern struct fstype devtmpfs_fstype;

#endif
