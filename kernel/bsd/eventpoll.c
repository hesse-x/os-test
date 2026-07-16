/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/eventpoll.h"

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"
#include <stddef.h>
#include <xos/epoll.h>
#include <xos/errno.h>
#include <xos/signal.h>
#include <xos/socket.h>

// copy_from_user/copy_to_user have no dedicated header; forward-declare.
size_t copy_from_user(void *dst, const void *src, size_t size);
size_t copy_to_user(void *dst, const void *src, size_t size);

// ===================== rbtree compare (by file*) =====================
static int ep_cmp(rb_node *a, rb_node *b) {
  epitem *ea = rb_entry(a, epitem, rb_node);
  epitem *eb = rb_entry(b, epitem, rb_node);
  if (ea->file < eb->file)
    return -1;
  if (ea->file > eb->file)
    return 1;
  return 0;
}

// ===================== callback: data arrived on monitored fd
// =====================
static void ep_poll_callback(wait_queue_t *wq, unsigned long flags) {
  __poll mask = (__poll)flags;
  epitem *epi = wq->data;
  eventpoll *ep = epi->ep;
  spin_lock(&ep->lock);
  if (epi->is_et) {
    // ET: enqueue only on first transition to ready
    if (!epi->is_ready) {
      epi->revents = mask & epi->events;
      if (epi->revents) {
        list_push_back(&ep->ready_list, &epi->rdllist_node);
        epi->is_ready = 1;
        __wake_up(&ep->wq, POLLIN);
      }
    }
  } else {
    // LT: refresh revents each time, ensure on ready_list
    epi->revents = mask & epi->events;
    if (epi->revents) {
      if (!epi->is_ready) {
        list_push_back(&ep->ready_list, &epi->rdllist_node);
        epi->is_ready = 1;
      }
      __wake_up(&ep->wq, POLLIN);
    }
  }
  spin_unlock(&ep->lock);
}

// Resolve which wait_queue_head a monitored file exposes for epoll waiters.
// Lazily allocates the per-type wq on first epoll registration so that
// data-ready wakeups can reach ep_poll_callback. Delegates to file_wq_get so
// sys_poll and sys_epoll_wait register waiters on the same per-type wq (and
// can't diverge — a divergence left sys_poll waiters on an unwoken wq).
static wait_queue_head *ep_target_wq(struct file *f) { return file_wq_get(f); }

// ===================== eventpoll lifecycle =====================
eventpoll *eventpoll_create(void) {
  eventpoll *ep = kmalloc(sizeof(eventpoll));
  if (!ep)
    return NULL;
  ep->lock = SPINLOCK_INIT;
  init_wait_queue_head(&ep->wq);
  ep->rbt = RB_ROOT;
  list_init(&ep->ready_list);
  ep->nitems = 0;
  return ep;
}

void eventpoll_release(eventpoll *ep) {
  epitem *epi_array[EP_MAX_ITEMS];
  int count = 0;

  // Phase 1: under ep->lock, remove all epitems from their target wait
  // queues, ready_list, and rbt.  Collect into epi_array for later cleanup.
  spin_lock(&ep->lock);
  rb_node *n = rb_first(&ep->rbt);
  while (n && count < EP_MAX_ITEMS) {
    epitem *epi = rb_entry(n, epitem, rb_node);
    wait_queue_head *wq = ep_target_wq(epi->file);
    if (wq)
      remove_wait_queue(wq, &epi->wait);
    if (epi->is_ready)
      list_remove(&epi->rdllist_node);
    rb_erase(&ep->rbt, n);
    epi_array[count++] = epi;
    n = rb_first(&ep->rbt);
  }
  ep->nitems = 0;
  spin_unlock(&ep->lock);

  // Phase 2: wait for any in-flight ep_poll_callback (collected before our
  // remove_wait_queue) to finish.  __wake_up wraps the callback in
  // rcu_read_lock, so synchronize_rcu ensures no callback is still using
  // these epitems.
  synchronize_rcu();

  // Phase 3: free all epitems.  No callback is in-flight at this point.
  for (int i = 0; i < count; i++) {
    file_put(epi_array[i]->file);
    kfree(epi_array[i]);
  }
  kfree(ep);
}

int ep_insert(eventpoll *ep, struct file *f, struct epoll_event *ev) {
  if (f->type == FD_EPOLL)
    return -EINVAL;
  if (ep->nitems >= EP_MAX_ITEMS)
    return -ENOMEM;
  epitem key = {.file = f};
  if (rb_search(&ep->rbt, &key.rb_node, ep_cmp))
    return -EEXIST;
  epitem *epi = kmalloc(sizeof(epitem));
  if (!epi)
    return -ENOMEM;
  epi->file = f;
  file_get(f);
  epi->events = ev->events & ~EPOLLET;
  epi->is_et = !!(ev->events & EPOLLET);
  epi->user_data = ev->data.u64;
  epi->revents = 0;
  epi->is_ready = 0;
  epi->ep = ep;
  epi->wait.func = ep_poll_callback;
  epi->wait.data = epi;
  list_init(&epi->rdllist_node);
  wait_queue_head *wq = ep_target_wq(f);
  if (wq) {
    add_wait_queue(wq, &epi->wait);
  } else {
    printk(LOG_WARN, "ep_insert: NO wq for fd_type=%d file=%p\n", f->type,
           (void *)f);
  }
  spin_lock(&ep->lock);
  rb_insert(&ep->rbt, &epi->rb_node, ep_cmp);
  ep->nitems++;
  // Immediate readiness check
  __poll revents = file_poll(f, epi->events);
  if (revents) {
    epi->revents = revents;
    list_push_back(&ep->ready_list, &epi->rdllist_node);
    epi->is_ready = 1;
  }
  spin_unlock(&ep->lock);
  if (revents)
    __wake_up(&ep->wq, POLLIN);
  return 0;
}

int ep_remove(eventpoll *ep, struct file *f) {
  epitem key = {.file = f};
  rb_node *node = rb_search(&ep->rbt, &key.rb_node, ep_cmp);
  if (!node)
    return -ENOENT;
  epitem *epi = rb_entry(node, epitem, rb_node);
  wait_queue_head *wq = ep_target_wq(f);
  if (wq)
    remove_wait_queue(wq, &epi->wait);
  // Wait for any in-flight ep_poll_callback (collected by __wake_up before our
  // remove_wait_queue) to finish.  __wake_up wraps the callback call in
  // rcu_read_lock, so synchronize_rcu() ensures the callback has completed.
  synchronize_rcu();
  spin_lock(&ep->lock);
  if (epi->is_ready)
    list_remove(&epi->rdllist_node);
  rb_erase(&ep->rbt, node);
  ep->nitems--;
  spin_unlock(&ep->lock);
  file_put(f);
  kfree(epi);
  return 0;
}

int ep_modify(eventpoll *ep, struct file *f, struct epoll_event *ev) {
  epitem key = {.file = f};
  rb_node *node = rb_search(&ep->rbt, &key.rb_node, ep_cmp);
  if (!node)
    return -ENOENT;
  epitem *epi = rb_entry(node, epitem, rb_node);
  spin_lock(&ep->lock);
  epi->events = ev->events & ~EPOLLET;
  epi->is_et = !!(ev->events & EPOLLET);
  epi->user_data = ev->data.u64;
  __poll revents = file_poll(f, epi->events);
  if (revents && !epi->is_ready) {
    epi->revents = revents;
    list_push_back(&ep->ready_list, &epi->rdllist_node);
    epi->is_ready = 1;
  } else if (!revents && epi->is_ready) {
    list_remove(&epi->rdllist_node);
    epi->is_ready = 0;
  }
  spin_unlock(&ep->lock);
  if (revents)
    __wake_up(&ep->wq, POLLIN);
  return 0;
}

// ===================== epoll_wait wake callback =====================
// Registered on ep->wq while sys_epoll_wait is blocking; wakes the caller.
static void ep_wait_callback(wait_queue_t *wq, unsigned long flags) {
  xtask *proc = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(proc);
}

// ===================== syscalls =====================
int64_t sys_epoll_create(int64_t size) {
  (void)size;
  return sys_epoll_create1(0);
}

int64_t sys_epoll_create1(int64_t flags) {
  if (flags & ~EPOLL_CLOEXEC)
    return -EINVAL;
  eventpoll *ep = eventpoll_create();
  if (!ep)
    return -ENOMEM;
  xtask *proc = current_task;
  spin_lock(&proc->proc->files->fd_lock);
  int fd = alloc_fd(proc->proc->files, 3);
  if (fd < 0) {
    spin_unlock(&proc->proc->files->fd_lock);
    kfree(ep);
    return -EMFILE;
  }
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(&proc->proc->files->fd_lock);
    kfree(ep);
    return -ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_EPOLL;
  f->epoll = ep;
  if (flags & EPOLL_CLOEXEC)
    f->flags |= FD_CLOEXEC;
  fd_install(proc->proc->files, fd, f);
  spin_unlock(&proc->proc->files->fd_lock);
  return fd;
}

int64_t sys_epoll_ctl(int64_t epfd, int64_t op, int64_t fd, int64_t ev_ptr) {
  xtask *proc = current_task;
  rcu_read_lock();
  struct file *ef = fd_lookup(proc->proc->files, (int)epfd);
  if (!ef || ef->type != FD_EPOLL) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(ef);
  rcu_read_unlock();
  eventpoll *ep = ef->epoll;

  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, (int)fd);
  if (!f) {
    rcu_read_unlock();
    file_put(ef);
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int ret;
  if (op != EPOLL_CTL_DEL) {
    struct epoll_event ev;
    if (copy_from_user(&ev, (void *)ev_ptr, sizeof(ev))) {
      ret = -EFAULT;
    } else if (op == EPOLL_CTL_ADD) {
      ret = ep_insert(ep, f, &ev);
    } else if (op == EPOLL_CTL_MOD) {
      ret = ep_modify(ep, f, &ev);
    } else {
      ret = -EINVAL;
    }
  } else {
    ret = ep_remove(ep, f);
  }
  file_put(f);
  file_put(ef);
  return ret;
}

int64_t sys_epoll_wait(int64_t epfd, int64_t ev_ptr, int64_t maxevents,
                       int64_t timeout_ms) {
  xtask *proc = current_task;
  rcu_read_lock();
  struct file *ef = fd_lookup(proc->proc->files, (int)epfd);
  if (!ef || ef->type != FD_EPOLL) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(ef);
  rcu_read_unlock();
  eventpoll *ep = ef->epoll;
  if (!ep) {
    file_put(ef);
    return -EBADF;
  }
  if (maxevents <= 0 || maxevents > EP_MAX_ITEMS) {
    file_put(ef);
    return -EINVAL;
  }

  uint64_t deadline =
      (timeout_ms > 0) ? sched_clock() + (uint64_t)timeout_ms * 1000000ULL : 0;

  // Register self on ep->wq so ep_poll_callback's __wake_up wakes us.
  wait_queue_t wait;
  wait.func = ep_wait_callback;
  wait.data = proc;
  list_init(&wait.node);
  add_wait_queue(&ep->wq, &wait);

  int n = 0;
  while (1) {
    // prepare_to_wait: 先标 BLOCKED 再持 ep->lock 查 ready_list，使
    // ep_poll_callback 的 __wake_up 若在重查后到达命中已 BLOCKED 的 task。
    proc->state = BLOCKED;
    proc->wait_event = WAIT_POLL;
    proc->wait_timed_out = 0;
    spin_lock(&ep->lock);
    if (!list_empty(&ep->ready_list)) {
      // Process only the items present at the start of this pass. LT items
      // that remain ready are re-enqueued to the tail but must not be
      // re-reported in the same epoll_wait call; capping the pass at the
      // original tail prevents looping on persistently-ready fds.
      list_node *pass_end = ep->ready_list.prev;
      list_node *it = ep->ready_list.next;
      while (it != &ep->ready_list && n < maxevents) {
        epitem *epi = LIST_ENTRY(it, epitem, rdllist_node);
        list_node *next = it->next;
        list_remove(it);
        epi->is_ready = 0;
        // LT: re-check readiness before reporting. A ready_list entry may be
        // stale (e.g. data was consumed after it was enqueued); if no longer
        // ready, skip it instead of reporting a spurious event.
        if (!epi->is_et) {
          __poll revents = file_poll(epi->file, epi->events);
          if (!revents) {
            it = next;
            continue; // stale, drop without reporting
          }
          epi->revents = revents;
          // Still ready: re-enqueue for subsequent epoll_wait calls (LT).
          list_push_back(&ep->ready_list, &epi->rdllist_node);
          epi->is_ready = 1;
        }
        struct epoll_event ev = {.events = epi->revents,
                                 .data = {.u64 = epi->user_data}};
        if (copy_to_user(&((struct epoll_event *)ev_ptr)[n], &ev, sizeof(ev))) {
          spin_unlock(&ep->lock);
          sched_cancel_spurious_wake(proc);
          remove_wait_queue(&ep->wq, &wait);
          file_put(ef);
          return -EFAULT;
        }
        n++;
        if (it == pass_end)
          break; // reached end of this pass
        it = next;
      }
      spin_unlock(&ep->lock);
      // prepare_to_wait: 循环顶部标过 BLOCKED，若 re-check 期间
      // ep_wait_callback 的 wake_wq_target 命中把 run_node push 进了
      // run_queue（state=READY），此 break 不 走 schedule() 会留下悬空
      // run_node。cancel 掉虚假唤醒：摘 run_node + state=RUNNING。
      sched_cancel_spurious_wake(proc);
      remove_wait_queue(&ep->wq, &wait);
      file_put(ef);
      return n;
    }
    spin_unlock(&ep->lock);

    if (timeout_ms == 0) {
      sched_cancel_spurious_wake(proc);
      remove_wait_queue(&ep->wq, &wait);
      file_put(ef);
      return 0;
    }

    // Block on WAIT_POLL
    uint64_t effective_deadline = deadline;
    if (effective_deadline == 0 && proc->alarm_deadline != 0) {
      // Indefinite wait but an alarm is armed: use it as the wake deadline so
      // the timer queue can fire SIGALRM (otherwise the alarm never triggers
      // while blocked in epoll_wait).
      effective_deadline = proc->alarm_deadline;
    }
    if (effective_deadline > 0) {
      uint64_t now = sched_clock();
      if (now >= effective_deadline) {
        sched_cancel_spurious_wake(proc);
        remove_wait_queue(&ep->wq, &wait);
        file_put(ef);
        return 0; // timeout
      }
      proc->wait_deadline = effective_deadline;
      uint64_t pflags;
      spin_lock_irqsave(&cpu_locals[proc->assigned_cpu].scheduler_lock,
                        &pflags);
      sched_timer_queue_insert(proc->assigned_cpu, proc);
      spin_unlock_irqrestore(&cpu_locals[proc->assigned_cpu].scheduler_lock,
                             pflags);
    } else {
      proc->wait_deadline = 0;
    }

    schedule();

    // EINTR check (signal priority over timeout), mirrors sys_poll
    {
      uint64_t pend =
          __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
      uint64_t deliv = pend & ~proc->proc->sig_blocked;
      deliv |= (pend & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
      if (deliv) {
        sched_cancel_spurious_wake(proc);
        remove_wait_queue(&ep->wq, &wait);
        file_put(ef);
        return -EINTR;
      }
    }

    if (proc->wait_timed_out && timeout_ms > 0) {
      sched_cancel_spurious_wake(proc);
      remove_wait_queue(&ep->wq, &wait);
      file_put(ef);
      return 0; // timeout
    }
    // Woken by data arrival — re-check ready_list
  }
}

int64_t sys_epoll_pwait(int64_t epfd, int64_t ev_ptr, int64_t maxevents,
                        int64_t timeout_ms, int64_t sigmask_ptr,
                        int64_t sigsetsize) {
  xtask *proc = current_task;
  sigset_t old_blocked = proc->proc->sig_blocked;
  sigset_t new_mask;
  int have_mask = 0;
  if (sigmask_ptr) {
    if (sigsetsize != sizeof(sigset_t))
      return -EINVAL;
    if (copy_from_user(&new_mask, (void *)sigmask_ptr, sizeof(new_mask)))
      return -EFAULT;
    proc->proc->sig_blocked = new_mask;
    proc->proc->sig_blocked |= ((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    have_mask = 1;
  }
  int64_t ret = sys_epoll_wait(epfd, ev_ptr, maxevents, timeout_ms);
  proc->proc->sig_blocked = old_blocked;
  (void)have_mask;
  return ret;
}
