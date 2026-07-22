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

struct dirent {
  ino_t d_ino;               /* inode number */
  off_t d_off;               /* offset of this entry */
  unsigned short d_reclen;   /* length of this record */
  char d_name[NAME_MAX + 1]; /* filename (null-terminated) */
};

typedef struct {
  int dd_fd;
  // Internal state managed by libc
} DIR;

LIBC_EXPORT DIR *opendir(const char *name);
LIBC_EXPORT struct dirent *readdir(DIR *dirp);
LIBC_EXPORT int closedir(DIR *dirp);
LIBC_EXPORT int scandir(const char *dirp, struct dirent ***namelist,
                        int (*filter)(const struct dirent *),
                        int (*compar)(const struct dirent **,
                                      const struct dirent **));
/* S05: directory stream positioning + dirfd. seekdir/telldir use the kernel
 * getdents d_off cookie via lseek on the dir fd; rewinddir resets to start. */
LIBC_EXPORT long telldir(DIR *dirp);
LIBC_EXPORT void seekdir(DIR *dirp, long loc);
LIBC_EXPORT void rewinddir(DIR *dirp);
LIBC_EXPORT int dirfd(DIR *dirp);
LIBC_EXPORT int readdir_r(DIR *dirp, struct dirent *entry,
                          struct dirent **result);

#ifdef __cplusplus
}
#endif

#endif /* _DIRENT_H */
