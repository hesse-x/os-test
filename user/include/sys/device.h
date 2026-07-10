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

struct dev_props {
  uint16_t bustype;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
  char name[64];
};

LIBC_EXPORT int device_set_meta(const char *name, const char *subsystem,
                                const char *devtype,
                                const struct dev_props *props);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_DEVICE_H */
