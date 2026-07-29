/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdint.h>
#include <syscall.h>

#include <sys/cdefs.h>

/* mremap/shm_unlink/__shm_mapname lived here before the mman module switched to
 * musl (musl_mman_objs builds src/mman/mremap.c + shm_open.c, which define them
 * upstream). Deleted to avoid a duplicate-definition clash at link. shm_open is
 * now also supplied by musl's shm_open.c (it was previously only declared, not
 * implemented, in this repo). */

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
