/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <syscall.h>
#include <unistd.h>

#include <sys/cdefs.h>
#include <sys/mman.h> // MREMAP_FIXED
#include <xos/errno.h>
#include <xos/mman.h>

enum { MUSL_NAME_MAX = 255 };

LIBC_EXPORT size_t strnlen(const char *s, size_t maxlen) {
  size_t len = 0;
  while (len < maxlen && s[len])
    len++;
  return len;
}

LIBC_EXPORT int prctl(int option, ...) {
  va_list ap;
  va_start(ap, option);
  uint64_t arg2 = va_arg(ap, uint64_t);
  uint64_t arg3 = va_arg(ap, uint64_t);
  uint64_t arg4 = va_arg(ap, uint64_t);
  uint64_t arg5 = va_arg(ap, uint64_t);
  va_end(ap);
  return sys_prctl(option, arg2, arg3, arg4, arg5);
}

LIBC_EXPORT void *mremap(void *old_addr, size_t old_size, size_t new_size,
                         int flags, ...) {
  void *new_addr = NULL;
  if (flags & MREMAP_FIXED) {
    va_list ap;
    va_start(ap, flags);
    new_addr = va_arg(ap, void *);
    va_end(ap);
  }
  return sys_mremap(old_addr, old_size, new_size, flags, new_addr);
}

char *__shm_mapname(const char *name, char *buf) {
  static const char prefix[] = "/dev/shm/";
  size_t len = 0;

  while (*name == "/"[0])
    name++;
  while (name[len] && name[len] != "/"[0])
    len++;
  if (!len || name[len] ||
      (len <= 2 && name[0] == "."[0] && name[len - 1] == "."[0])) {
    errno = EINVAL;
    return NULL;
  }
  if (len > MUSL_NAME_MAX) {
    errno = ENAMETOOLONG;
    return NULL;
  }

  for (size_t i = 0; i < sizeof(prefix) - 1; i++)
    buf[i] = prefix[i];
  for (size_t i = 0; i <= len; i++)
    buf[sizeof(prefix) - 1 + i] = name[i];
  return buf;
}

LIBC_EXPORT int shm_unlink(const char *name) {
  char path[MUSL_NAME_MAX + sizeof("/dev/shm/")];
  if (!__shm_mapname(name, path))
    return -1;
  return unlink(path);
}
