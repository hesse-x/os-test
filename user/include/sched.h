/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SCHED_H
#define _SCHED_H

// User-space shim for <sched.h>. Shadows the host glibc sched.h, whose
// struct timespec / cpu_set_t / etc. clash with this OS's own <time.h>.
// This OS only exposes sched_yield(); the host header is not needed.

#ifdef __cplusplus
extern "C" {
#endif

int sched_yield(void);

#ifdef __cplusplus
}
#endif

#endif /* _SCHED_H */
