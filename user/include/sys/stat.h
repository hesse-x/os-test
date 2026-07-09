/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <xos/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Userspace struct stat — mirrors struct kstat from xos/stat.h.
 * Must match Linux x86-64 ABI exactly (144 bytes). */
struct stat {
  dev_t st_dev;            /* offset  0: uint64_t */
  ino_t st_ino;            /* offset  8: uint64_t */
  nlink_t st_nlink;        /* offset 16: uint64_t */
  mode_t st_mode;          /* offset 24: uint32_t */
  uid_t st_uid;            /* offset 28: uint32_t */
  gid_t st_gid;            /* offset 32: uint32_t */
  int __pad0;              /* offset 36: padding  */
  dev_t st_rdev;           /* offset 40: uint64_t */
  off_t st_size;           /* offset 48: int64_t  */
  blksize_t st_blksize;    /* offset 56: int64_t  */
  blkcnt_t st_blocks;      /* offset 64: int64_t  */
  struct timespec st_atim; /* offset 72: 16 bytes */
  struct timespec st_mtim; /* offset 88: 16 bytes */
  struct timespec st_ctim; /* offset 104: 16 bytes */
  long __reserved[3];      /* offset 120: 24 bytes */
};

/* Compile-time assertion: struct stat and struct kstat must have identical
 * layout. We check key offsets that previously diverged (st_size). */
#if defined(__cplusplus)
static_assert(offsetof(struct stat, st_size) == offsetof(struct kstat, st_size),
              "struct stat and struct kstat st_size offset mismatch");
static_assert(sizeof(struct stat) == sizeof(struct kstat),
              "struct stat and struct kstat size mismatch");
#else
_Static_assert(offsetof(struct stat, st_size) ==
                   offsetof(struct kstat, st_size),
               "struct stat and struct kstat st_size offset mismatch");
_Static_assert(sizeof(struct stat) == sizeof(struct kstat),
               "struct stat and struct kstat size mismatch");
#endif

/* File type constants (S_IFMT..S_IFIFO), permission bits (S_ISUID..S_IXOTH),
 * and type tests (S_ISREG/S_ISDIR/S_ISCHR/S_ISBLK) come from xos/stat.h
 * (included above) — single source of truth shared with the kernel. */

LIBC_EXPORT int stat(const char *path, struct stat *st);
LIBC_EXPORT int fstat(int fd, struct stat *st);
LIBC_EXPORT int mkdir(const char *path, mode_t mode);
LIBC_EXPORT int mknod(const char *path, mode_t mode, dev_t dev);
LIBC_EXPORT int chmod(const char *path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STAT_H */
