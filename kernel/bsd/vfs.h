/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stdint.h>

void vfs_init(void);

int64_t sys_open(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                 int64_t unused2, int64_t unused3);
int64_t sys_stat(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4);
int64_t sys_mkdir(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4);
int64_t sys_unlink(int64_t arg1, int64_t unused1, int64_t unused2,
                   int64_t unused3, int64_t unused4, int64_t unused5);
int64_t sys_rmdir(int64_t arg1, int64_t unused1, int64_t unused2,
                  int64_t unused3, int64_t unused4, int64_t unused5);
int64_t sys_dev_create(int64_t arg1, int64_t arg2, int64_t unused1,
                       int64_t unused2, int64_t unused3, int64_t unused4);
int64_t sys_getdents(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                     int64_t unused2, int64_t unused3);

#endif
