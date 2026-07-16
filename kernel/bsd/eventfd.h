/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_EVENTFD_H
#define KERNEL_BSD_EVENTFD_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/xcore/spinlock.h"

#define EFD_SEMAPHORE 0x1
#define EFD_NONBLOCK 0x800
#define EFD_CLOEXEC 0x8000
#define EVENTFD_MAX 0xFFFFFFFFFFFFFFFEULL

typedef struct eventfd_ctx {
  uint64_t count;
  uint32_t flags;
  spinlock lock;
} eventfd_ctx;

int64_t sys_eventfd2(int64_t initval, int64_t flags);

struct file;
int64_t eventfd_do_read(struct file *f, void *buf);
int64_t eventfd_do_write(struct file *f, const void *buf, size_t len);

// ISR-safe signal: increment ctx->count under irqsave and wake pollers.
// Callable from hard-IRQ context (xHCI ISR).  No copy_from_user, no
// current_task, no schedule().  ctx->lock MUST be taken irqsave here and
// everywhere else ctx->lock is taken (see evdev_refact.md §5.2).
void eventfd_signal_isr(struct file *f);

#endif // KERNEL_BSD_EVENTFD_H
