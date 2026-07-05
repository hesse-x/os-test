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

#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE 64 // (1 << FUTEX_HASH_BITS)

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

struct futex_bucket {
  list_node waiters; // waiter thread list (linked via proc->futex_node)
  spinlock lock;
};

extern struct futex_bucket futex_table[FUTEX_HASH_SIZE];

int64_t sys_futex(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                  int64_t arg5, int64_t arg6);

#endif // KERNEL_BSD_FUTEX_H
