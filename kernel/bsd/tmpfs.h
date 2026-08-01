/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_BSD_TMPFS_H
#define KERNEL_BSD_TMPFS_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/bsd/fops.h"
#include "kernel/bsd/mount.h"

struct inode;
extern struct fstype tmpfs_fstype;
extern const struct file_operations tmpfs_file_fops;
// Kernel-space read of a tmpfs regular file (execve dispatches here via
// vfs_read_kernel by fstype).
int tmpfs_read_kern(struct inode *ip, uint64_t offset, void *buf, size_t count);

#endif
