/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM 0x0002
#define GRND_INSECURE 0x0004

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
LIBC_EXPORT int getentropy(void *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_RANDOM_H */
