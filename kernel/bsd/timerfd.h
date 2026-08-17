/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_TIMERFD_H
#define KERNEL_BSD_TIMERFD_H

#include <stdint.h>

#include "kernel/xcore/list.h"
#include "kernel/xcore/spinlock.h"

// *_CLOEXEC must equal O_CLOEXEC (02000000, kernel/bsd/kfcntl.h) — musl's
// <sys/timerfd.h> defines TFD_CLOEXEC = O_CLOEXEC and passes it raw through
// the syscall. The old 0x8000 (the kernel-internal FD_CLOEXEC fd-bit) made
// sys_timerfd_create reject standard callers with -EINVAL.
#define TFD_CLOEXEC 02000000
#define TFD_NONBLOCK 0x800
#define TFD_TIMER_ABSTIME 0x01

typedef struct timerfd_ctx {
  uint64_t expiry;   // sched_clock() ns, 0 = disarmed
  uint64_t interval; // 0 = one-shot
  uint64_t ticks;
  spinlock lock;
  list_node node; // linked into the global timerfd_list
  struct file *f;
} timerfd_ctx;

int64_t sys_timerfd_create(int64_t clockid, int64_t flags);
int64_t sys_timerfd_settime(int64_t fd, int64_t flags, int64_t new_ptr,
                            int64_t old_ptr);
void timerfd_tick_all(void); // called from timer IRQ
int64_t timerfd_do_read(struct file *f, void *buf);
void timerfd_init(void); // initialize global list + lock (call once at boot)

// Global timerfd list lock (defined in timerfd.c); proc.c file_put uses it to
// detach a closing timerfd from the list.
extern spinlock timerfd_list_lock;

#endif // KERNEL_BSD_TIMERFD_H
