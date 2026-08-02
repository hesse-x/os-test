/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_STATFS_ABI_H
#define KERNEL_BSD_STATFS_ABI_H

#include <stdint.h>

/*
 * struct statfs — Linux x86-64 ABI (asm-generic/statfs.h + glibc
 * <bits/statfs.h>).  Field order and types match the x86-64 glibc layout
 * exactly (__fsword_t == long, __fsblkcnt_t == unsigned long, __fsid_t ==
 * int[2]), so llvm-libc's statfs_utils (which includes the host
 * <asm/statfs.h>) copies the same bytes the kernel writes here.
 *
 * Kernel-side mirror of the Linux ABI. Userspace uses musl's
 * <sys/statfs.h> definition.
 *
 * Capacity fields (f_blocks/f_bfree/f_bavail/f_files/f_ffree) are returned
 * as 0 by this kernel today: FAT32 keeps no free-cluster counter and
 * llvm-libc's pathconf() only consumes f_type/f_bsize/f_frsize/f_namelen
 * (see doc/design/todo.md).  They exist so future df-style tooling can be
 * wired up without an ABI change.
 */
struct statfs {
  long f_type;            /* filesystem type magic */
  long f_bsize;           /* optimal transfer block size */
  unsigned long f_blocks; /* total data blocks */
  unsigned long f_bfree;  /* free data blocks */
  unsigned long f_bavail; /* free blocks for unprivileged users */
  unsigned long f_files;  /* total file nodes */
  unsigned long f_ffree;  /* free file nodes */
  int f_fsid[2];          /* filesystem id (__kernel_fsid_t) */
  long f_namelen;         /* maximum filename length */
  long f_frsize;          /* fragment size */
  long f_flags;           /* mount flags */
  long f_spare[4];        /* reserved */
};

/* Filesystem magic numbers (linux/magic.h) — used by statfs f_type. */
#define MSDOS_SUPER_MAGIC 0x4d44 /* FAT (FAT32 root fs) */
#define TMPFS_MAGIC 0x01021997   /* tmpfs / devtmpfs */
#define SYSFS_MAGIC 0x62656572   /* sysfs */

#endif /* KERNEL_BSD_STATFS_ABI_H */
