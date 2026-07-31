/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H

/* Linux UAPI futex op constants — the kernel side (kernel/bsd/futex.c) mirrors
 * Linux's futex(2) ABI (SYS_FUTEX 202, FUTEX_WAIT/WAKE). Published as a UAPI
 * header so cross-built consumers (e.g. libc++'s <atomic> wait/wake, which
 * #include <linux/futex.h> and syscall(SYS_futex, ..., FUTEX_WAIT_PRIVATE,
 * ...)) resolve the constants. Keep aligned with Linux
 * include/uapi/linux/futex.h. */

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_FD 2
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI 12

#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_BITSET_MATCH_ANY 0xffffffff

/* Convenience combinations used by glibc/libc++ callers (Linux
 * include/uapi/linux/futex.h). */
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_REQUEUE_PRIVATE (FUTEX_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_CMP_REQUEUE_PRIVATE (FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG)

#endif /* _LINUX_FUTEX_H */
