/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_EVENTFD_H
#define _SYS_EVENTFD_H

#include <stdint.h>
#include <sys/cdefs.h>

#define EFD_SEMAPHORE 0x1
#define EFD_NONBLOCK 0x800
#define EFD_CLOEXEC 0x8000

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT int eventfd(unsigned int initval, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_EVENTFD_H */
