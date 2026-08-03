/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "arch/x64/smp.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h" // current_task + struct xtask (remove_wait_queue diagnostic)

void init_wait_queue_head(wait_queue_head *wq) {
  wq->lock = SPINLOCK_INIT;
  list_init(&wq->head);
}

void add_wait_queue(wait_queue_head *wq, wait_queue_t *wait) {
  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  list_push_back(&wq->head, &wait->node);
  spin_unlock_irqrestore(&wq->lock, flags);
}

void remove_wait_queue(wait_queue_head *wq, wait_queue_t *wait) {
  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  if (wait->node.prev == &wait->node && wait->node.next == &wait->node)
    printk(LOG_WARN, "remove_wait_queue: self-ref node=%p wq=%p pid=%d\n",
           (void *)&wait->node, (void *)wq, current_task->pid);
  list_remove(&wait->node);
  spin_unlock_irqrestore(&wq->lock, flags);
}

// Traverse wq->head under wq->lock and call callbacks in-loop. This cures the
// stack-allocated wait-node UAF: wait nodes usually live on the caller's stack
// (sys_epoll_wait/timerfd/signalfd/eventfd/poll/ring), and remove_wait_queue
// takes the same lock, so a waiter cannot detach its node during the callback —
// the node stays valid. Lock order (doc/design/kernel/epoll.md):
//   A: waiter callback takes scheduler_lock (wq->lock → scheduler_lock; no
//      reverse edge — scheduler_lock holders never call wait_queue ops);
//   B: ep_poll_callback holds ep->lock then nests __wake_up(&ep->wq):
//      W_fd → ep->lock → W_ep → scheduler_lock, acyclic.
// Nested __wake_up targets a different wq (not a self-relock); each irqsave
// level keeps its own stack flags. flags is opaque to xcore (bsd callbacks
// define the poll-mask semantics).
//
// EPOLLEXCLUSIVE unicast: after waking one exclusive waiter, skip the remaining
// exclusive waiters but still wake all non-exclusive ones (matches Linux
// __wake_up_common) — anti-thundering-herd for many processes/epolls sharing a
// listen socket.
void __wake_up(wait_queue_head *wq, unsigned long flags) {
  if (__builtin_expect(!wq, 0))
    panic("__wake_up: NULL wq, caller=%p", __builtin_return_address(0));
  uint64_t irqflags;
  spin_lock_irqsave(&wq->lock, &irqflags);
  list_node *it = wq->head.next;
  int woken_exclusive = 0;
  while (it != &wq->head) {
    wait_queue_t *wq_entry = LIST_ENTRY(it, wait_queue_t, node);
    // Save next first: the callback (wake_with_event) only flips
    // wq_entry->state without unlinking; unlinking is done solely by
    // remove_wait_queue, which must wait for our lock release — so the list
    // stays stable while we walk.
    list_node *next = it->next;
    // Defense: a self-referential node (next==self) is a detached node not in
    // any list. It can't appear in a healthy list; seeing one means a caller
    // left a stack wait node on the wq and the stack was reused (see the
    // sys_poll wait leak fix). Continuing would spin forever on it==next and
    // wedge the box, so warn and abort this wake — the fix is to never leave
    // nodes behind.
    if (next == it) {
      WARN_ON_ONCE(1);
      break;
    }
    if (wq_entry->exclusive) {
      if (woken_exclusive) {
        // Already woke one exclusive waiter; skip the rest
        // (anti-thundering-herd).
        it = next;
        continue;
      }
      if (wq_entry->func)
        wq_entry->func(wq_entry, flags);
      woken_exclusive = 1;
    } else {
      if (wq_entry->func)
        wq_entry->func(wq_entry, flags);
    }
    it = next;
  }
  spin_unlock_irqrestore(&wq->lock, irqflags);
}
