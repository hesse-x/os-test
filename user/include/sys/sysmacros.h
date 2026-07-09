/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#include <sys/cdefs.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* makedev / major / minor — Linux 64-bit dev_t encoding.
 * Canonical glibc form: each operand is cast to dev_t (64-bit) before the
 * shift, so the ~0xfff / ~0xff masks (which are int) do not force a 32-bit
 * intermediate and trigger -Werror=shift-count-overflow on the << 32. */
static inline dev_t makedev(unsigned int major, unsigned int minor) {
  return ((dev_t)(major & 0xfff) << 8) | ((dev_t)(major & ~0xfff) << 32) |
         ((dev_t)(minor & 0xff)) | ((dev_t)(minor & ~0xff) << 12);
}

static inline unsigned int major(dev_t dev) {
  return ((dev >> 8) & 0xfff) | ((dev >> 32) & 0xfffff000);
}

static inline unsigned int minor(dev_t dev) {
  return (dev & 0xff) | ((dev >> 12) & 0xffffff00);
}

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SYSMACROS_H */
