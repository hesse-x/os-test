/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "kernel/xcore/wait_queue.h"

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
  list_remove(&wait->node);
  spin_unlock_irqrestore(&wq->lock, flags);
}

// 遍历时【不持】wq->lock：先收集待唤醒项到本地数组，解锁后逐个回调，
// 避免 ep_poll_callback 内取 eventpoll->lock + __wake_up(&ep->wq) 锁嵌套。
// flags 透传给每个回调，xcore 不解释其含义（poll 掩码语义由 bsd 回调解释）。
void __wake_up(wait_queue_head *wq, unsigned long flags) {
  wait_queue_t *targets[32];
  int n = 0;
  uint64_t irqflags;
  spin_lock_irqsave(&wq->lock, &irqflags);
  list_node *it = wq->head.next;
  while (it != &wq->head && n < 32) {
    wait_queue_t *wq_entry = LIST_ENTRY(it, wait_queue_t, node);
    targets[n++] = wq_entry;
    it = it->next;
  }
  spin_unlock_irqrestore(&wq->lock, irqflags);
  for (int i = 0; i < n; i++) {
    if (targets[i]->func)
      targets[i]->func(targets[i], flags);
  }
}
