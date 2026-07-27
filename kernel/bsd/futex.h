/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_FUTEX_H
#define KERNEL_BSD_FUTEX_H

#include "kernel/xcore/list.h"
#include "kernel/xcore/spinlock.h"
#include <stdint.h>
#include <xos/types.h> // pid_t

#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE 64 // (1 << FUTEX_HASH_BITS)

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

// Robust-futex word layout (kernel-internal masks). The low 30 bits hold the
// owning TID; bit 30 is FUTEX_OWNER_DIED (set by the kernel on exit if the
// owner died holding the lock, defined in <xos/robust_list.h> as it is
// user-visible ABI).
#define FUTEX_TID_MASK 0x3fffffff
// Cap on robust-list walk length to bound kernel time spent in exit on a
// pathological or hostile list.
#define ROBUST_LIST_LIMIT 2048

struct futex_bucket {
  list_node waiters; // waiter thread list (linked via proc->futex_node)
  spinlock lock;
};

extern struct futex_bucket futex_table[FUTEX_HASH_SIZE];

int64_t sys_futex(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                  int64_t arg5, int64_t arg6);

// Robust-futex list (musl pthread robust mutexes). set_robust_list(2) records a
// per-thread head; on exit the kernel walks it, marks still-held locks with
// FUTEX_OWNER_DIED, and wakes one waiter.
struct proc; // forward — full def in kernel/bsd/proc.h
int64_t sys_set_robust_list(int64_t head, int64_t len, int64_t, int64_t,
                            int64_t, int64_t);
int64_t sys_get_robust_list(int64_t pid, int64_t head_ptr, int64_t len_ptr,
                            int64_t, int64_t, int64_t);
void exit_robust_list(struct proc *bp, pid_t owner_tid);

#endif // KERNEL_BSD_FUTEX_H
