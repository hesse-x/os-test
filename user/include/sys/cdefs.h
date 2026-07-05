/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H

/* Public ABI symbol annotation. libc.so is compiled with -fvisibility=hidden;
 * only declarations marked LIBC_EXPORT are exported; internal symbols are
 * automatically hidden. Static libraries ignore visibility. */
#define LIBC_EXPORT __attribute__((visibility("default")))

#endif /* _SYS_CDEFS_H */
