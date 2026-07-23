/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/signal.h"
#include "kernel/bsd/signalfd.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"

#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/signal.h>

// copy_from_user/copy_to_user have no dedicated header; forward-declare.
size_t copy_from_user(void *dst, const void *src, size_t size);
size_t copy_to_user(void *dst, const void *src, size_t size);

// Wake a blocked signalfd read waiter (registered on f->wq).
static void signalfd_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *proc = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(proc);
}

uint64_t proc_signalfd_pending(signalfd_ctx *sfd) {
  proc *bp = current_task->proc;
  if (!bp || !bp->signal)
    return 0;
  uint64_t pend = __atomic_load_n(&bp->sig_pending, __ATOMIC_ACQUIRE);
  pend |= bp->signal->shared_pending;
  return pend & sfd->sigmask;
}

// Scan the process fd table for a signalfd that accepts `signo` and whose
// signal is not blocked. Returns 1 if such a fd exists (the signal should be
// left pending for the signalfd reader instead of delivered to a handler).
//
// The fd_table can be mutated concurrently by sibling threads sharing this
// files_struct (close/dup2 on another CPU: fd_uninstall + synchronize_rcu +
// file_put frees the file). Hold an RCU read-side critical section across
// the scan so a file can't be freed while we dereference it — same protocol
// as every other fd_table reader (fd_lookup / RCU_DEREFERENCE).
int signalfd_consumes(proc *bp, int signo) {
  if (!bp || !bp->files)
    return 0;
  uint64_t bit = 1ULL << (signo - 1);
  int consumes = 0;
  rcu_read_lock();
  for (int fd = 0; fd < MAX_FD; fd++) {
    struct file *f = fd_lookup(bp->files, fd);
    if (f && f->type == FD_SIGNALFD && f->signalfd) {
      signalfd_ctx *ctx = f->signalfd;
      if ((ctx->sigmask & bit) && !(bp->sig_blocked & bit)) {
        consumes = 1;
        break;
      }
    }
  }
  rcu_read_unlock();
  return consumes;
}

// Return the wait_queue_head of the first signalfd accepting `signo`.
// Caller MUST hold rcu_read_lock across this call and the use of the
// returned wq — otherwise the file (and its wq) can be closed/freed
// concurrently by a sibling thread sharing files.
wait_queue_head *signalfd_wq(proc *bp, int signo) {
  if (!bp || !bp->files)
    return NULL;
  uint64_t bit = 1ULL << (signo - 1);
  for (int fd = 0; fd < MAX_FD; fd++) {
    struct file *f = fd_lookup(bp->files, fd);
    if (f && f->type == FD_SIGNALFD && f->signalfd) {
      signalfd_ctx *ctx = f->signalfd;
      if (ctx->sigmask & bit)
        return file_wq_get(f);
    }
  }
  return NULL;
}

int64_t sys_signalfd4(int64_t fd, int64_t sigmask_ptr, int64_t sizemask,
                      int64_t flags) {
  if (sizemask != sizeof(sigset_t))
    return -EINVAL;
  if (flags & ~(SFD_CLOEXEC | SFD_NONBLOCK))
    return -EINVAL;

  uint64_t mask;
  if (copy_from_user(&mask, (void *)sigmask_ptr, sizeof(mask)))
    return -EFAULT;
  // SIGKILL/SIGSTOP cannot be caught via signalfd (they always default-act).
  mask &= ~((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP)));

  xtask *proc = current_task;
  if (fd == -1) {
    // Create a new signalfd.
    signalfd_ctx *ctx = (signalfd_ctx *)kmalloc(sizeof(signalfd_ctx));
    if (!ctx)
      return -ENOMEM;
    ctx->sigmask = mask;
    ctx->lock = SPINLOCK_INIT;
    spin_lock(&proc->proc->files->fd_lock);
    int newfd = alloc_fd(proc->proc->files, 3);
    if (newfd < 0) {
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
    f->type = FD_SIGNALFD;
    f->signalfd = ctx;
    if (flags & SFD_NONBLOCK)
      f->flags |= O_NONBLOCK;
    fd_install(proc->proc->files, newfd, f);
    fd_set_cloexec(proc->proc->files, newfd, (flags & SFD_CLOEXEC) ? 1 : 0);
    spin_unlock(&proc->proc->files->fd_lock);
    return newfd;
  } else {
    // Update the mask of an existing signalfd.
    struct file *f = fd_lookup(proc->proc->files, (int)fd);
    if (!f || f->type != FD_SIGNALFD)
      return -EBADF;
    signalfd_ctx *ctx = f->signalfd;
    spin_lock(&ctx->lock);
    ctx->sigmask = mask;
    spin_unlock(&ctx->lock);
    return fd;
  }
}

// Blocking read of one signalfd_siginfo (128 bytes). Returns sizeof(siginfo)
// on success, negative errno on failure.
int64_t signalfd_do_read(struct file *f, void *buf) {
  signalfd_ctx *ctx = f->signalfd;
  if (!ctx)
    return -EBADF;

  wait_queue_head *wq = file_wq_get(f);
  wait_queue_t wait;
  if (wq) {
    wait.func = signalfd_wake_cb;
    wait.data = current_task;
    wait.exclusive = 0;
    list_init(&wait.node);
    add_wait_queue(wq, &wait);
  }

  int64_t ret;
  for (;;) {
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_POLL;
    current_task->wait_timed_out = 0;
    proc *bp = current_task->proc;
    uint64_t blocked = bp->sig_blocked;
    // Pending = private | shared, intersected with the fd mask. SIGKILL/SIGSTOP
    // bypass sig_blocked (always deliverable).
    uint64_t raw_priv =
        __atomic_load_n(&bp->sig_pending, __ATOMIC_ACQUIRE) & ctx->sigmask;
    uint64_t priv = raw_priv & ~blocked;
    priv |= (raw_priv & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
    uint64_t shared = 0;
    if (bp->signal) {
      uint64_t sflags;
      spin_lock_irqsave(&bp->signal->sig_lock, &sflags);
      shared = bp->signal->shared_pending & ctx->sigmask & ~blocked;
      spin_unlock_irqrestore(&bp->signal->sig_lock, sflags);
    }

    int signo = 0;
    uint64_t source = 0; // 1 = private, 2 = shared
    if (priv) {
      signo = __builtin_ctzll(priv) + 1; // bit index = signo-1
      source = 1;
    } else if (shared) {
      signo = __builtin_ctzll(shared) + 1; // bit index = signo-1
      source = 2;
    }

    if (signo > 0) {
      signalfd_siginfo si;
      __memset(&si, 0, sizeof(si));
      si.ssi_signo = (uint32_t)signo;
      si.ssi_code = SI_USER;
      // Consume the pending bit.
      if (source == 1) {
        __atomic_and_fetch(&bp->sig_pending, ~(1ULL << (signo - 1)),
                           __ATOMIC_RELEASE);
      } else {
        uint64_t sflags;
        spin_lock_irqsave(&bp->signal->sig_lock, &sflags);
        bp->signal->shared_pending &= ~(1ULL << (signo - 1));
        spin_unlock_irqrestore(&bp->signal->sig_lock, sflags);
      }
      if (copy_to_user(buf, &si, sizeof(si)))
        ret = -EFAULT;
      else
        ret = (int64_t)sizeof(si);
      current_task->state = RUNNING;
      goto out;
    }

    if (f->flags & O_NONBLOCK) {
      current_task->state = RUNNING;
      ret = -EAGAIN;
      goto out;
    }
    // EINTR check (mirror epoll_wait)
    {
      uint64_t pend = __atomic_load_n(&bp->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t deliv = pend & ~bp->sig_blocked;
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
