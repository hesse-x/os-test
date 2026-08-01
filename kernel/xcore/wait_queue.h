/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_WAIT_QUEUE_H
#define KERNEL_WAIT_QUEUE_H

#include "kernel/xcore/list.h"
#include "kernel/xcore/spinlock.h"

typedef struct wait_queue_head {
  spinlock lock;
  list_node head; // links wait_queue_t.node
} wait_queue_head;

#define WAIT_QUEUE_HEAD_INIT(name)                                             \
  {                                                                            \
    SPINLOCK_INIT, { &(name).head, &(name).head }                              \
  }

// Pure synchronization primitive: func's flags is opaque pass-through data from
// the caller; xcore does not interpret it. The event-mask semantics for
// poll/epoll are interpreted by the bsd-layer callback (see
// file_poll.h/eventpoll.c).
struct wait_queue_t;
typedef void (*wait_queue_func_t)(struct wait_queue_t *wq, unsigned long flags);

typedef struct wait_queue_t {
  list_node node;
  wait_queue_func_t func;
  void *data;    // usually points to an epitem
  int exclusive; // 1 = exclusive waiter: __wake_up wakes one then skips the
                 // rest (non-exclusive are all woken), to avoid thundering
                 // herd. Stack wait nodes must be explicitly initialized (0 or
                 // 1); don't leave stack residue.
} wait_queue_t;

void init_wait_queue_head(wait_queue_head *wq);
void add_wait_queue(wait_queue_head *wq, wait_queue_t *wait);
void remove_wait_queue(wait_queue_head *wq, wait_queue_t *wait);
void __wake_up(wait_queue_head *wq, unsigned long flags);

#endif // KERNEL_WAIT_QUEUE_H
