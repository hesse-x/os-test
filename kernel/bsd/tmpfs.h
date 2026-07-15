/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KERNEL_BSD_TMPFS_H
#define KERNEL_BSD_TMPFS_H

#include "kernel/bsd/fops.h"
#include "kernel/bsd/mount.h"

extern struct fstype tmpfs_fstype;
extern const struct file_operations tmpfs_file_fops;

#endif
