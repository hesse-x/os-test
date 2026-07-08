/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

#include <arch/x64/memlayout.h> /* IWYU pragma: keep */

#define PAGESIZE PAGE_SIZE

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#endif /* _SYS_PARAM_H */
