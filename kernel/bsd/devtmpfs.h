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
#include "kernel/xcore/atomic.h"   // refcount_t (§5: dev_ops 引用计数)
#include "kernel/xcore/xtask.h"    // pid_t

struct inode;

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

  /* §5: ops 生命周期引用计数(FUSE fuse_conn 式)。ops 脱离 inode/fd 引用
   * 计数独立计数:devtmpfs_create 取注册引用,devtmpfs_open 取 fd 引用,
   * file_put/cleanup_pid 放引用,归 0 才 kfree。仅 user-space driver
   * (driver_pid>0,kmalloc ops)会归 0;kernel device ops 为 static,注册引用
   * 永在,refcount 永不归 0。fd 持引用覆盖 read/write/ioctl/poll 裸读 i_priv
   * 的生命周期,故那些路径不必逐个 get/put。*/
  refcount_t refcount;

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

/* §5: dev_ops 引用计数(FUSE fuse_conn 式);见 devtmpfs.c。*/
void dev_ops_get(struct dev_ops *ops);
void dev_ops_put(struct dev_ops *ops);
/* §5: 锁下读 inode->i_priv 返回 ops 指针(不加引用);调用方持 fd 引用,
 * ops 在本 fd close 前不归 0,读出后可安全用。防 borrow-window UAF。*/
struct dev_ops *dev_ops_peek_by_inode(struct inode *ip);

extern struct fstype devtmpfs_fstype;

#endif
