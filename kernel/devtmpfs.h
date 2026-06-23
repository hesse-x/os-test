#ifndef KERNEL_DEVTMPFS_H
#define KERNEL_DEVTMPFS_H

#include <stdint.h>
#include <sys/types.h>
#include "kernel/inode.h"

struct dev_ops {
    pid_t    driver_pid;
    uint32_t device_type;  /* DEV_KBD, DEV_KMS, etc. */
};

void    devtmpfs_init(void);
struct inode *devtmpfs_lookup(const char *name);
int     devtmpfs_create(const char *name, int dev_type, struct dev_ops *ops);
uint64_t devtmpfs_open(const char *name, int flags);
void    devtmpfs_cleanup_pid(pid_t pid);

#endif
