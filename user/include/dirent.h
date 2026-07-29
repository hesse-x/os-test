/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/cdefs.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAME_MAX 255

/* Layout is identical to musl's struct dirent and to the kernel's
 * struct dirent64 (include/uapi/xos/dirent.h): readdir() returns a pointer
 * straight into the DIR's getdents buffer, so the on-the-wire record layout
 * and this struct must never diverge. */
struct dirent {
  ino_t d_ino;               /* inode number */
  off_t d_off;               /* offset of this entry (seekdir cookie) */
  unsigned short d_reclen;   /* length of this record */
  unsigned char d_type;      /* entry type (DT_DIR/DT_REG/...) */
  char d_name[NAME_MAX + 1]; /* filename (null-terminated) */
};

/* Opaque handle; the real layout (struct __dirstream) lives in musl's
 * src/dirent/__dirent.h and is not part of the public ABI. */
typedef struct __dirstream DIR;

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14

LIBC_EXPORT DIR *opendir(const char *name);
LIBC_EXPORT struct dirent *readdir(DIR *dirp);
LIBC_EXPORT int closedir(DIR *dirp);
LIBC_EXPORT DIR *fdopendir(int fd);
LIBC_EXPORT int scandir(const char *dirp, struct dirent ***namelist,
                        int (*filter)(const struct dirent *),
                        int (*compar)(const struct dirent **,
                                      const struct dirent **));
/* seekdir/telldir use the kernel getdents d_off cookie via lseek on the dir
 * fd; rewinddir resets to start. fdopendir wraps an already-open dir fd. */
LIBC_EXPORT long telldir(DIR *dirp);
LIBC_EXPORT void seekdir(DIR *dirp, long loc);
LIBC_EXPORT void rewinddir(DIR *dirp);
LIBC_EXPORT int dirfd(DIR *dirp);
LIBC_EXPORT int readdir_r(DIR *dirp, struct dirent *entry,
                          struct dirent **result);

/* GNU extension: gated on _GNU_SOURCE so translation units that supply their
 * own static versionsort (e.g. libinput's libinput-versionsort.h, built without
 * _GNU_SOURCE) keep using it without a "static follows non-static" clash. */
#if defined(_GNU_SOURCE)
LIBC_EXPORT int versionsort(const struct dirent **, const struct dirent **);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _DIRENT_H */
