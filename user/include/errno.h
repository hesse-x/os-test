/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _ERRNO_H
#define _ERRNO_H

#include <sys/cdefs.h>
#include <xos/errno.h>

#ifdef __cplusplus
extern "C" {
#endif

// errno is returned via __errno_location() as a TLS pointer, per-thread.
// Under dynamic linking this avoids the main ELF and libc.so each holding an
// errno copy that diverge on write/read
// (R_X86_64_COPY copies libc.so's errno into the main ELF .bss; libc.so
//  internally writes its own .bss errno on syscall failure, while the main
//  ELF reads its own COPY replica and always gets 0).
// TLS mode: errno lives in the TLS template; the main ELF and libc.so share
// the same instance of the current thread.
LIBC_EXPORT int *__errno_location(void);
#define errno (*__errno_location())

#ifdef __cplusplus
}
#endif

#endif /* _ERRNO_H */
