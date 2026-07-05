#ifndef KERNEL_DEVTMPFS_H
#define KERNEL_DEVTMPFS_H

#include "kernel/bsd/inode.h"
#include "kernel/xcore/xtask.h" // pid_t
#include <stdbool.h>
#include <stdint.h>

typedef int64_t ssize_t;
typedef uint32_t __poll_t;

struct xxtask_t;

struct dev_ops {
  pid_t driver_pid; // 0 = kernel device, >0 = user-space driver
  bool is_block;    // true = block device, false = char device

  // VFS callbacks (only called when driver_pid == 0)
  int (*open)(xtask_t *proc, int fd);
  int (*close)(xtask_t *proc, int fd);
  long (*ioctl)(uint32_t cmd, void *arg);
  uint64_t (*mmap)(xtask_t *proc, uint64_t size);
  ssize_t (*read)(xtask_t *proc, int fd, void *buf, size_t count);
  ssize_t (*write)(xtask_t *proc, int fd, const void *buf, size_t count);
  __poll_t (*poll)(xtask_t *proc, int events);
};

void devtmpfs_init(void);
int devtmpfs_create(const char *name, struct dev_ops *ops, struct shm *shm);
uint64_t devtmpfs_open(xtask_t *proc, const char *name, int flags);
struct inode *devtmpfs_lookup(const char *name);
void devtmpfs_cleanup_pid(pid_t pid);
void devtmpfs_remove(const char *name);

#endif
