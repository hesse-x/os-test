/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_DEVICE_H
#define _SYS_DEVICE_H

#include <sys/cdefs.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int device_register(const char *name);
LIBC_EXPORT int device_register_shm(const char *name, int shm_fd,
                                    uint32_t minor);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_DEVICE_H */
