/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/eventfd.h"

#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"
#include <stddef.h>
#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/signal.h>
#include <xos/socket.h>

// copy_from_user/copy_to_user have no dedicated header; forward-declare.
size_t copy_from_user(void *dst, const void *src, size_t size);
size_t copy_to_user(void *dst, const void *src, size_t size);

// Wake a blocked eventfd read/write waiter (registered on f->wq).
static void eventfd_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *proc = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(proc);
}

int64_t sys_eventfd2(int64_t initval, int64_t flags) {
  if (flags & ~(EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE))
    return -EINVAL;
  if ((uint64_t)initval > EVENTFD_MAX)
    return -EINVAL;

  eventfd_ctx *ctx = (eventfd_ctx *)kmalloc(sizeof(eventfd_ctx));
  if (!ctx)
    return -ENOMEM;
  ctx->count = (uint64_t)initval;
  ctx->flags = (uint32_t)flags;
  ctx->lock = SPINLOCK_INIT;

  xtask *proc = current_task;
  spin_lock(&proc->proc->files->fd_lock);
  int fd = alloc_fd(proc->proc->files, 3);
  if (fd < 0) {
    spin_unlock(&proc->proc->files->fd_lock);
    kfree(ctx);
    return -EMFILE;
  }
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(&proc->proc->files->fd_lock);
    kfree(ctx);
    return -ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_EVENTFD;
  f->eventfd = ctx;
  if (flags & EFD_NONBLOCK)
    f->flags |= O_NONBLOCK;
  fd_install(proc->proc->files, fd, f);
  fd_set_cloexec(proc->proc->files, fd, (flags & EFD_CLOEXEC) ? 1 : 0);
  spin_unlock(&proc->proc->files->fd_lock);
  return fd;
}

// Blocking read of 8 bytes. Returns 8 on success, negative errno on failure.
int64_t eventfd_do_read(struct file *f, void *buf) {
  eventfd_ctx *ctx = f->eventfd;
  if (!ctx)
    return -EBADF;

  uint64_t flags;
  wait_queue_head *wq = file_wq_get(f);
  wait_queue_t wait;
  if (wq) {
    wait.func = eventfd_wake_cb;
    wait.data = current_task;
    list_init(&wait.node);
    add_wait_queue(wq, &wait);
  }

  int64_t ret;
  for (;;) {
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_POLL;
    current_task->wait_timed_out = 0;
    spin_lock_irqsave(&ctx->lock, &flags);
    if (ctx->count > 0) {
      uint64_t val;
      if (ctx->flags & EFD_SEMAPHORE) {
        ctx->count--;
        val = 1;
      } else {
        val = ctx->count;
        ctx->count = 0;
      }
      spin_unlock_irqrestore(&ctx->lock, flags);
      if (copy_to_user(buf, &val, 8))
        ret = -EFAULT;
      else
        ret = 8;
      if (wq)
        __wake_up(wq, POLLOUT);
      current_task->state = RUNNING;
      goto out;
    }
    spin_unlock_irqrestore(&ctx->lock, flags);
    if (f->flags & O_NONBLOCK) {
      current_task->state = RUNNING;
      ret = -EAGAIN;
      goto out;
    }
    // EINTR check (mirror epoll_wait)
    {
      xtask *p = current_task;
      uint64_t pend = __atomic_load_n(&p->proc->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t deliv = pend & ~p->proc->sig_blocked;
      deliv |= (pend & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
      if (deliv) {
        current_task->state = RUNNING;
        ret = -EINTR;
        goto out;
      }
    }
    schedule();
  }

out:
  // prepare_to_wait: 循环顶部标过 BLOCKED，若 break 前被 wake 命中把 run_node
  // push 进 了 run_queue（state=READY），goto out 不走 schedule() 会留下悬空
  // run_node。cancel 掉。
  sched_cancel_spurious_wake(current_task);
  if (wq)
    remove_wait_queue(wq, &wait);
  return ret;
}

// Blocking write of 8 bytes. Returns 8 on success, negative errno on failure.
int64_t eventfd_do_write(struct file *f, const void *buf, size_t len) {
  eventfd_ctx *ctx = f->eventfd;
  if (!ctx)
    return -EBADF;
  if (len != 8)
    return -EINVAL;

  uint64_t val;
  if (copy_from_user(&val, buf, 8))
    return -EFAULT;
  if (val > EVENTFD_MAX)
    return -EINVAL;

  uint64_t flags;
  wait_queue_head *wq = file_wq_get(f);
  wait_queue_t wait;
  if (wq) {
    wait.func = eventfd_wake_cb;
    wait.data = current_task;
    list_init(&wait.node);
    add_wait_queue(wq, &wait);
  }

  int64_t ret;
  for (;;) {
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_POLL;
    current_task->wait_timed_out = 0;
    spin_lock_irqsave(&ctx->lock, &flags);
    if (ctx->count <= EVENTFD_MAX - val) {
      ctx->count += val;
      spin_unlock_irqrestore(&ctx->lock, flags);
      if (wq)
        __wake_up(wq, POLLIN);
      current_task->state = RUNNING;
      ret = 8;
      goto out;
    }
    spin_unlock_irqrestore(&ctx->lock, flags);
    if (f->flags & O_NONBLOCK) {
      current_task->state = RUNNING;
      ret = -EAGAIN;
      goto out;
    }
    {
      xtask *p = current_task;
      uint64_t pend = __atomic_load_n(&p->proc->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t deliv = pend & ~p->proc->sig_blocked;
      deliv |= (pend & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
      if (deliv) {
        current_task->state = RUNNING;
        ret = -EINTR;
        goto out;
      }
    }
    schedule();
  }

out:
  // prepare_to_wait: 循环顶部标过 BLOCKED，若 break 前被 wake 命中把 run_node
  // push 进 了 run_queue（state=READY），goto out 不走 schedule() 会留下悬空
  // run_node。cancel 掉。
  sched_cancel_spurious_wake(current_task);
  if (wq)
    remove_wait_queue(wq, &wait);
  return ret;
}

// ISR-safe signal: increment ctx->count under irqsave and wake pollers.
// Callable from hard-IRQ context (xHCI ISR).  No copy_from_user, no
// current_task, no schedule().  ctx->lock MUST be taken irqsave here and
// everywhere else ctx->lock is taken (see evdev_refact.md §5.2).
void eventfd_signal_isr(struct file *f) {
  eventfd_ctx *ctx = f->eventfd;
  if (!ctx)
    return;
  uint64_t flags;
  spin_lock_irqsave(&ctx->lock, &flags);
  if (ctx->count < EVENTFD_MAX) // saturate, never overflow
    ctx->count++;
  spin_unlock_irqrestore(&ctx->lock, flags);
  wait_queue_head *wq = file_wq_get(f);
  if (wq)
    __wake_up(wq, POLLIN); // __wake_up is ISR-safe (wait_queue.c)
}
