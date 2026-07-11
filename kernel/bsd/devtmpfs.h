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
#include "kernel/xcore/xtask.h" // pid_t

struct shm;

typedef int64_t ssize_t;
typedef uint32_t __poll;

struct sysfs_node;

struct dev_ops {
  pid_t driver_pid; // 0 = kernel device, >0 = user-space driver
  bool is_block;    // true = block device, false = char device
  uint32_t minor;   // device minor number (ioctl req routing)

  char subsystem[8];            // "input" / "drm" / "block" / "tty"
  char devtype[8];              // "evdev" / "card" / "disk" / "ptmx"
  void *subsys_priv;            // -> input_dev_props* / NULL
  struct sysfs_node *sysfs_dir; // sysfs 子树根 (移除时用)

  // VFS callbacks (only called when driver_pid == 0)
  int (*open)(xtask *proc, int fd);
  int (*close)(xtask *proc, int fd);
  long (*ioctl)(uint32_t cmd, void *arg);
  uint64_t (*mmap)(xtask *proc, uint64_t size);
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
