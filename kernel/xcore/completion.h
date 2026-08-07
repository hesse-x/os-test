/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_COMPLETION_H
#define KERNEL_XCORE_COMPLETION_H

#include <stdint.h>

#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"

typedef struct completion {
  spinlock lock;
  uint32_t done;
  wait_queue_head waiters;
} completion;

void init_completion(completion *c);
void reinit_completion(completion *c);
void complete(completion *c);
void complete_all(completion *c);
int wait_for_completion_timeout(completion *c, uint64_t timeout_ns);

#endif
