/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int mount(const char *source, const char *target,
                      const char *fstype, unsigned long flags,
                      const void *data);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MOUNT_H */
