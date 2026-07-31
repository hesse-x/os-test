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
/* 内核态读 tmpfs 普通文件(execve 经 vfs_read_kernel 按 fstype 分发调用)。 */
int tmpfs_read_kern(struct inode *ip, uint64_t offset, void *buf, size_t count);

#endif
