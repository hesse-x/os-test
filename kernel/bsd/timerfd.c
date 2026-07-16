/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/timerfd.h"

#include "arch/x64/apic.h"
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
#include "xos/signal.h"
#include <stddef.h>
#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/socket.h>
#include <xos/time.h>

// Global list of armed timerfd contexts (scanned by timerfd_tick_all from the
// timer IRQ). Protected by timerfd_list_lock.
static list_node timerfd_list;
spinlock timerfd_list_lock;

// copy_from_user/copy_to_user have no dedicated header; forward-declare.
size_t copy_from_user(void *dst, const void *src, size_t size);
size_t copy_to_user(void *dst, const void *src, size_t size);

struct itimerspec {
  struct timespec it_interval;
  struct timespec it_value;
};

void timerfd_init(void) {
  list_init(&timerfd_list);
  timerfd_list_lock = SPINLOCK_INIT;
}

// Wake a blocked timerfd read waiter (registered on f->wq).
static void timerfd_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *proc = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(proc);
}

int64_t sys_timerfd_create(int64_t clockid, int64_t flags) {
  if (clockid != CLOCK_MONOTONIC)
    return -EINVAL;
  if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK))
    return -EINVAL;

  timerfd_ctx *tfd = (timerfd_ctx *)kmalloc(sizeof(timerfd_ctx));
  if (!tfd)
    return -ENOMEM;
  tfd->expiry = 0;
  tfd->interval = 0;
  tfd->ticks = 0;
  tfd->lock = SPINLOCK_INIT;
  list_init(&tfd->node);
  tfd->f = NULL;

  xtask *proc = current_task;
  spin_lock(&proc->proc->files->fd_lock);
  int fd = alloc_fd(proc->proc->files, 3);
  if (fd < 0) {
    spin_unlock(&proc->proc->files->fd_lock);
    kfree(tfd);
    return -EMFILE;
  }
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(&proc->proc->files->fd_lock);
    kfree(tfd);
    return -ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_TIMERFD;
  f->timerfd = tfd;
  tfd->f = f;
  if (flags & TFD_CLOEXEC)
    f->flags |= FD_CLOEXEC;
  if (flags & TFD_NONBLOCK)
    f->flags |= O_NONBLOCK;
  fd_install(proc->proc->files, fd, f);
  spin_unlock(&proc->proc->files->fd_lock);

  uint64_t tfd_flags;
  spin_lock_irqsave(&timerfd_list_lock, &tfd_flags);
  list_push_back(&timerfd_list, &tfd->node);
  spin_unlock_irqrestore(&timerfd_list_lock, tfd_flags);
  return fd;
}

int64_t sys_timerfd_settime(int64_t fd, int64_t flags, int64_t new_ptr,
                            int64_t old_ptr) {
  xtask *proc = current_task;
  struct file *f = fd_lookup(proc->proc->files, (int)fd);
  if (!f || f->type != FD_TIMERFD)
    return -EBADF;
  timerfd_ctx *tfd = f->timerfd;
  if (!tfd)
    return -EBADF;

  struct itimerspec neu;
  if (copy_from_user(&neu, (void *)new_ptr, sizeof(neu)))
    return -EFAULT;

  uint64_t tfd_settime_flags;
  spin_lock_irqsave(&tfd->lock, &tfd_settime_flags);
  if (old_ptr) {
    struct itimerspec old;
    old.it_interval.tv_sec = (time_t)(tfd->interval / 1000000000ULL);
    old.it_interval.tv_nsec = (long)(tfd->interval % 1000000000ULL);
    old.it_value.tv_sec = (time_t)(tfd->expiry / 1000000000ULL);
    old.it_value.tv_nsec = (long)(tfd->expiry % 1000000000ULL);
    if (copy_to_user((void *)old_ptr, &old, sizeof(old))) {
      spin_unlock_irqrestore(&tfd->lock, tfd_settime_flags);
      return -EFAULT;
    }
  }
  uint64_t now = sched_clock();
  if (neu.it_value.tv_sec == 0 && neu.it_value.tv_nsec == 0) {
    tfd->expiry = 0;
  } else if (flags & TFD_TIMER_ABSTIME) {
    tfd->expiry = (uint64_t)neu.it_value.tv_sec * 1000000000ULL +
                  (uint64_t)neu.it_value.tv_nsec;
  } else {
    tfd->expiry = now + (uint64_t)neu.it_value.tv_sec * 1000000000ULL +
                  (uint64_t)neu.it_value.tv_nsec;
  }
  tfd->interval = (uint64_t)neu.it_interval.tv_sec * 1000000000ULL +
                  (uint64_t)neu.it_interval.tv_nsec;
  spin_unlock_irqrestore(&tfd->lock, tfd_settime_flags);
  return 0;
}

void timerfd_tick_all(void) {
  uint64_t now = sched_clock();
  spin_lock(&timerfd_list_lock);
  list_node *it = timerfd_list.next;
  while (it != &timerfd_list) {
    timerfd_ctx *tfd = LIST_ENTRY(it, timerfd_ctx, node);
    it = it->next;
    if (tfd->expiry && now >= tfd->expiry) {
      spin_lock(&tfd->lock);
      tfd->ticks++;
      if (tfd->interval)
        tfd->expiry = now + tfd->interval;
      else
        tfd->expiry = 0;
      spin_unlock(&tfd->lock);
      wait_queue_head *wq = file_wq_get(tfd->f);
      if (wq)
        __wake_up(wq, POLLIN);
    }
  }
  spin_unlock(&timerfd_list_lock);
}

// Blocking read of 8 bytes (tick count). Returns 8 on success.
int64_t timerfd_do_read(struct file *f, void *buf) {
  timerfd_ctx *tfd = f->timerfd;
  if (!tfd)
    return -EBADF;

  wait_queue_head *wq = file_wq_get(f);
  wait_queue_t wait;
  if (wq) {
    wait.func = timerfd_wake_cb;
    wait.data = current_task;
    list_init(&wait.node);
    add_wait_queue(wq, &wait);
  }

  int64_t ret;
  for (;;) {
    uint64_t flags;
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_POLL;
    current_task->wait_timed_out = 0;
    spin_lock_irqsave(&tfd->lock, &flags);
    if (tfd->ticks > 0) {
      uint64_t val = tfd->ticks;
      tfd->ticks = 0;
      spin_unlock_irqrestore(&tfd->lock, flags);
      if (copy_to_user(buf, &val, 8))
        ret = -EFAULT;
      else
        ret = 8;
      current_task->state = RUNNING;
      goto out;
    }
    spin_unlock_irqrestore(&tfd->lock, flags);
    if (f->flags & O_NONBLOCK) {
      current_task->state = RUNNING;
      ret = -EAGAIN;
      goto out;
    }
    {
      xtask *p = current_task;
      uint64_t pend = __atomic_load_n(&p->proc->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t deliv = pend & ~p->proc->sig_blocked;
      deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
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
