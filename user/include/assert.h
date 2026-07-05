/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _ASSERT_H
#define _ASSERT_H

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT void __assert_fail(const char *expr, const char *file, int line);

#ifdef __cplusplus
}
#endif

#define assert(expr)                                                           \
  ((void)((expr) || (__assert_fail(#expr, __FILE__, __LINE__), 0)))

#endif /* _ASSERT_H */
