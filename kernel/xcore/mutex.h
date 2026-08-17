/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_MUTEX_H
#define KERNEL_XCORE_MUTEX_H

#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"

// A non-recursive, sleepable mutex for process-context critical sections.
typedef struct mutex {
  spinlock guard;
  int locked;
  wait_queue_head waiters;
} mutex;

void mutex_init(mutex *m);
void mutex_lock(mutex *m);
void mutex_unlock(mutex *m);

#endif // KERNEL_XCORE_MUTEX_H
