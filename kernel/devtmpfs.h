#ifndef KERNEL_DEVTMPFS_H
#define KERNEL_DEVTMPFS_H

#include <stdint.h>
#include "kernel/proc.h"    // pid_t
#include "kernel/inode.h"

typedef int64_t ssize_t;
typedef uint32_t __poll_t;

struct task_t;

struct dev_ops {
    pid_t    driver_pid;     // 0 = kernel device, >0 = user-space driver
    uint32_t device_type;    // DEV_KMS, DEV_SERIAL, etc.

    // VFS callbacks (only called when driver_pid == 0)
    int      (*open)(struct task_t *proc, int fd);
    int      (*close)(struct task_t *proc, int fd);
    long     (*ioctl)(uint32_t cmd, void *arg);
    uint64_t (*mmap)(struct task_t *proc, uint64_t size);
    ssize_t  (*read)(struct task_t *proc, int fd, void *buf, size_t count);
    ssize_t  (*write)(struct task_t *proc, int fd, const void *buf, size_t count);
    __poll_t (*poll)(struct task_t *proc, int events);
};

void    devtmpfs_init(void);
int     devtmpfs_create(const char *name, int dev_type, struct dev_ops *ops);
uint64_t devtmpfs_open(struct task_t *proc, const char *name, int flags);
struct inode *devtmpfs_lookup(const char *name);
void    devtmpfs_cleanup_pid(pid_t pid);
void    devtmpfs_remove(const char *name);
pid_t   isr_lookup_driver(uint32_t dev_type);

#endif
