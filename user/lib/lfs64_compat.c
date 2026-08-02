/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * x86_64 already uses 64-bit off_t and struct stat.  Keep the explicit LFS
 * entry points because some hosted compilers redirect calls to them when
 * _FILE_OFFSET_BITS=64 is enabled, even though musl's headers alias the names.
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>

#include <sys/stat.h>
#include <sys/types.h>

#define LFS64_EXPORT __attribute__((visibility("default")))

LFS64_EXPORT int open64(const char *path, int flags, ...) {
  mode_t mode = 0;

  if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }

  return open(path, flags, mode);
}

LFS64_EXPORT int fcntl64(int fd, int cmd, ...) {
  unsigned long arg;
  va_list ap;

  va_start(ap, cmd);
  arg = va_arg(ap, unsigned long);
  va_end(ap);
  return fcntl(fd, cmd, arg);
}

LFS64_EXPORT FILE *fopen64(const char *restrict path,
                           const char *restrict mode) {
  return fopen(path, mode);
}

LFS64_EXPORT int lstat64(const char *restrict path, struct stat *restrict st) {
  return lstat(path, st);
}
