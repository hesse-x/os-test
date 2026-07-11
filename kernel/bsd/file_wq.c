/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "kernel/bsd/eventpoll.h" // struct eventpoll (ep->wq)
#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/inode.h" // struct inode (wq field)
#include "kernel/bsd/sysfs.h" // ringbuf_fops
#include "kernel/bsd/types.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/wait_queue.h"

// 惰性分配：若 f->wq==NULL 则 kmalloc + init。返回 wq 指针。
//
// ringbuf-backed FD_DEV 使用 per-inode wq（同一 inode 的所有 fd 实例共享），
// 使得用户态驱动写入 SHM ring 后可以通过 ioctl RINGBUF_WAKE 唤醒所有消费者。
wait_queue_head *file_wq_get(struct file *f) {
  /* ringbuf-backed device: use per-inode wq (shared across fd instances) */
  if (f->type == FD_DEV && f->f_op == &ringbuf_fops && f->inode) {
    if (f->inode->wq)
      return f->inode->wq;
    wait_queue_head *wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
    if (!wq)
      return NULL;
    init_wait_queue_head(wq);
    f->inode->wq = wq;
    return wq;
  }
  /* epoll fd: use ep->wq so ep_poll_callback's __wake_up(&ep->wq) wakes poll */
  if (f->type == FD_EPOLL && f->epoll) {
    return &f->epoll->wq;
  }
  /* Generic per-file wq */
  if (f->wq)
    return f->wq;
  wait_queue_head *wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
  if (!wq)
    return NULL;
  init_wait_queue_head(wq);
  f->wq = wq;
  return wq;
}
