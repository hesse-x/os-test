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

#ifdef __cplusplus
}
#endif

#endif /* _DIRENT_H */
