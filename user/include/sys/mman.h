/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <xos/mman.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT void *mmap(void *addr, size_t length, int prot, int flags, int fd,
                       uint64_t offset);
LIBC_EXPORT int munmap(void *addr, size_t length);
LIBC_EXPORT int memfd_create(const char *name, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H */
