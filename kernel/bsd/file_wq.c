/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/wait_queue.h"

// 惰性分配：若 f->wq==NULL 则 kmalloc + init。返回 wq 指针。
wait_queue_head *file_wq_get(struct file *f) {
  if (f->wq)
    return f->wq;
  wait_queue_head *wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
  if (!wq)
    return NULL;
  init_wait_queue_head(wq);
  f->wq = wq;
  return wq;
}
