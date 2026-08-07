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
#include "kernel/xcore/atomic.h"   // refcount_t (§5: dev_ops refcount)
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h" // pid_t

struct inode;
struct file;
struct dev_mmap_request;
struct dev_mmap_backing;

// Linux 64-bit dev_t encoding (mirrors user sysmacros.h; pure arithmetic, no
// deps).
static inline uint64_t k_makedev(uint32_t major, uint32_t minor) {
  return ((uint64_t)(major & 0xfff) << 8) | ((uint64_t)(major & ~0xfff) << 32) |
         ((uint64_t)(minor & 0xff)) | ((uint64_t)(minor & ~0xff) << 12);
}

// Decode side (splits statx stx_dev/stx_rdev into major/minor), inverse of
// k_makedev.
static inline uint32_t k_major(uint64_t dev) {
  return (uint32_t)(((dev >> 8) & 0xfff) | ((dev >> 32) & 0xfffff000));
}

static inline uint32_t k_minor(uint64_t dev) {
  return (uint32_t)((dev & 0xff) | ((dev >> 12) & 0xffffff00));
}

enum dev_ops_storage {
  DEV_OPS_STATIC = 0,
  DEV_OPS_HEAP,
  DEV_OPS_OWNER_EMBEDDED,
};

struct dev_ops {
  pid_t driver_pid; // 0 = kernel device, >0 = user-space driver
  bool is_block;    // true = block device, false = char device
  uint32_t minor;   // device minor number (ioctl req routing)

  // Independent registration/fd references. storage/release determines what
  // happens when the last reference is dropped after node detach.
  refcount_t refcount;
  enum dev_ops_storage storage;
  void (*release)(struct dev_ops *ops);

  char subsystem[8]; // "input" / "drm" / "block" / "tty"
  char devtype[8];   // "evdev" / "card" / "disk" / "ptmx"
  void *subsys_priv; // -> input_dev_props* / NULL
  void *uevent_priv; // -> uevent_attr_priv* (sysfs uevent attr priv) / NULL
  struct sysfs_node *sysfs_dir; // sysfs subtree root (used on removal)

  // VFS callbacks (only called when driver_pid == 0)
  int (*open)(xtask *proc, int fd);
  int (*close)(xtask *proc, int fd);
  long (*ioctl)(uint32_t cmd, void *arg);
  uint64_t (*mmap)(xtask *proc, uint64_t size, uint64_t offset);
  ssize_t (*read)(xtask *proc, int fd, void *buf, size_t count);
  ssize_t (*write)(xtask *proc, int fd, const void *buf, size_t count);
  __poll (*poll)(xtask *proc, int events);

  // OFD-aware callbacks for devices whose state must survive dup/fork and
  // SCM_RIGHTS. When present these take precedence over the legacy callbacks.
  int (*open_file)(xtask *proc, struct file *file);
  int (*close_file)(xtask *proc, struct file *file);
  long (*ioctl_file)(xtask *proc, struct file *file, uint32_t cmd, void *arg);
  uint64_t (*mmap_file)(xtask *proc, struct file *file, uint64_t size,
                        uint64_t offset);
  int (*mmap_prepare_file)(struct file *file,
                           const struct dev_mmap_request *request,
                           struct dev_mmap_backing *backing);
  ssize_t (*read_file)(xtask *proc, struct file *file, void *buf, size_t count);
  __poll (*poll_file)(xtask *proc, struct file *file, int events);
  wait_queue_head *(*wait_queue_file)(struct file *file);
};

void devtmpfs_init(void);
int devtmpfs_mkdir(const char *name);
int devtmpfs_create(const char *name, struct dev_ops *ops, struct shm *shm);
uint64_t devtmpfs_open(xtask *proc, const char *name, int flags,
                       struct mount_entry *m);
struct inode *devtmpfs_lookup(const char *name);
void devtmpfs_cleanup_pid(pid_t pid);
void devtmpfs_remove(const char *name);

// §5: dev_ops refcount (FUSE fuse_conn style); see devtmpfs.c.
void dev_ops_get(struct dev_ops *ops);
void dev_ops_put(struct dev_ops *ops);
extern const struct vma_owner_ops dev_ops_vma_owner_ops;
// §5: under lock, reads inode->i_priv and returns ops (no ref taken); caller
// holds fd ref so ops won't reach 0 before this fd closes. Prevents
// borrow-window UAF.
struct dev_ops *dev_ops_peek_by_inode(struct inode *ip);
// Reverse map: device inode → registered /dev/<name> (borrowed; stable while
// the device is registered). NULL if not a registered devtmpfs device.
const char *devtmpfs_name_by_inode(struct inode *ip);

extern struct fstype devtmpfs_fstype;

#endif
