/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/eventpoll.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/signal.h" // signal_struct (alarm_deadline / sig_lock)
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"

#include <xos/epoll.h>
#include <xos/errno.h>
#include <xos/signal.h>
#include <xos/socket.h>

// copy_from_user/copy_to_user have no dedicated header; forward-declare.
size_t copy_from_user(void *dst, const void *src, size_t size);
size_t copy_to_user(void *dst, const void *src, size_t size);

// Serializes interest-tree changes and each file's reverse registration list.
// Readiness delivery remains protected by the per-eventpoll lock.
static spinlock epoll_ctl_lock = SPINLOCK_INIT;

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
  uint64_t irqflags;
  spin_lock_irqsave(&ep->lock, &irqflags);
  // EPOLLONESHOT: after the single ready report (epoll_wait path sets
  // is_disarmed), stop re-enqueuing on subsequent wakeups until EPOLL_CTL_MOD
  // clears is_disarmed. Holds ep->lock, races only with ep_modify (also under
  // ep->lock) — no tearing.
  if (epi->is_disarmed) {
    spin_unlock_irqrestore(&ep->lock, irqflags);
    return;
  }
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
  spin_unlock_irqrestore(&ep->lock, irqflags);
}

// Resolve which wait_queue_head a monitored file exposes for epoll waiters.
// Lazily allocates the per-type wq on first epoll registration so that
// data-ready wakeups can reach ep_poll_callback. Delegates to file_wq_get so
// sys_poll and sys_epoll_wait register waiters on the same per-type wq (and
// can't diverge — a divergence left sys_poll waiters on an unwoken wq).
static wait_queue_head *ep_target_wq(struct file *f) { return file_wq_get(f); }

// Linux permits epoll-on-epoll while rejecting cycles and overly deep nests.
static bool ep_graph_reaches(eventpoll *from, eventpoll *target, int depth);

static bool ep_tree_reaches(rb_node *node, eventpoll *target, int depth) {
  if (!node)
    return false;
  epitem *item = rb_entry(node, epitem, rb_node);
  if (item->file->type == FD_EPOLL && item->file->epoll &&
      ep_graph_reaches(item->file->epoll, target, depth + 1))
    return true;
  return ep_tree_reaches(node->rb_left, target, depth) ||
         ep_tree_reaches(node->rb_right, target, depth);
}

static bool ep_graph_reaches(eventpoll *from, eventpoll *target, int depth) {
  if (from == target || depth >= 4)
    return true;
  return ep_tree_reaches(from->rbt.rb_node, target, depth);
}

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
  while (1) {
    spin_lock(&epoll_ctl_lock);
    rb_node *n = rb_first(&ep->rbt);
    if (!n) {
      spin_unlock(&epoll_ctl_lock);
      break;
    }
    epitem *epi = rb_entry(n, epitem, rb_node);
    if (epi->target_wq)
      remove_wait_queue(epi->target_wq, &epi->wait);
    synchronize_rcu();
    uint64_t ep_flags;
    spin_lock_irqsave(&ep->lock, &ep_flags);
    if (epi->is_ready)
      list_remove(&epi->rdllist_node);
    rb_erase(&ep->rbt, &epi->rb_node);
    ep->nitems--;
    spin_unlock_irqrestore(&ep->lock, ep_flags);
    list_remove(&epi->file_node);
    spin_unlock(&epoll_ctl_lock);

    file_put(epi->file);
    kfree(epi);
  }
  kfree(ep);
}

int ep_insert(eventpoll *ep, struct file *f, struct files *owner, int fd,
              struct epoll_event *ev) {
  epitem *epi = kmalloc(sizeof(epitem));
  if (!epi)
    return -ENOMEM;
  epi->file = f;
  epi->owner = owner;
  epi->fd = fd;
  file_get(f);
  // Mode flags (EPOLLET/EPOLLONESHOT/EPOLLEXCLUSIVE) live on dedicated flags,
  // not in the event mask — events stores only real poll events (IN/OUT/...).
  epi->events = ev->events & ~(EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE);
  epi->is_et = !!(ev->events & EPOLLET);
  epi->is_oneshot = !!(ev->events & EPOLLONESHOT);
  epi->is_disarmed = 0;
  epi->is_exclusive = !!(ev->events & EPOLLEXCLUSIVE);
  epi->user_data = ev->data.u64;
  epi->revents = 0;
  epi->is_ready = 0;
  epi->ep = ep;
  epi->wait.func = ep_poll_callback;
  epi->wait.data = epi;
  epi->wait.exclusive = epi->is_exclusive;
  list_init(&epi->rdllist_node);
  list_init(&epi->file_node);
  epi->target_wq = ep_target_wq(f);
  if (!epi->target_wq) {
    printk(LOG_WARN, "ep_insert: NO wq for fd_type=%d file=%p\n", f->type,
           (void *)f);
  }

  spin_lock(&epoll_ctl_lock);
  if (f->type == FD_EPOLL && ep_graph_reaches(f->epoll, ep, 0)) {
    spin_unlock(&epoll_ctl_lock);
    file_put(f);
    kfree(epi);
    return -ELOOP;
  }
  // The fd may have been closed after sys_epoll_ctl took its temporary file
  // reference. Do not create an interest that can never be auto-removed.
  if (atomic_read(&f->fd_refs) == 0) {
    spin_unlock(&epoll_ctl_lock);
    file_put(f);
    kfree(epi);
    return -EBADF;
  }
  uint64_t ep_flags;
  spin_lock_irqsave(&ep->lock, &ep_flags);
  if (ep->nitems >= EP_MAX_ITEMS) {
    spin_unlock_irqrestore(&ep->lock, ep_flags);
    spin_unlock(&epoll_ctl_lock);
    file_put(f);
    kfree(epi);
    return -ENOMEM;
  }
  epitem key = {.file = f};
  if (rb_search(&ep->rbt, &key.rb_node, ep_cmp)) {
    spin_unlock_irqrestore(&ep->lock, ep_flags);
    spin_unlock(&epoll_ctl_lock);
    file_put(f);
    kfree(epi);
    return -EEXIST;
  }
  if (!f->epoll_items_initialized) {
    list_init(&f->epoll_items);
    f->epoll_items_initialized = 1;
  }
  list_push_back(&f->epoll_items, &epi->file_node);
  if (epi->target_wq)
    add_wait_queue(epi->target_wq, &epi->wait);
  rb_insert(&ep->rbt, &epi->rb_node, ep_cmp);
  ep->nitems++;
  // Immediate readiness check
  __poll revents = file_poll(f, epi->events);
  if (revents) {
    epi->revents = revents;
    list_push_back(&ep->ready_list, &epi->rdllist_node);
    epi->is_ready = 1;
  }
  spin_unlock_irqrestore(&ep->lock, ep_flags);
  spin_unlock(&epoll_ctl_lock);
  if (revents)
    __wake_up(&ep->wq, POLLIN);
  return 0;
}

static void ep_remove_item(epitem *epi) {
  eventpoll *ep = epi->ep;
  if (epi->target_wq)
    remove_wait_queue(epi->target_wq, &epi->wait);
  synchronize_rcu();
  uint64_t ep_flags;
  spin_lock_irqsave(&ep->lock, &ep_flags);
  if (epi->is_ready)
    list_remove(&epi->rdllist_node);
  rb_erase(&ep->rbt, &epi->rb_node);
  ep->nitems--;
  spin_unlock_irqrestore(&ep->lock, ep_flags);
  list_remove(&epi->file_node);
}

int ep_remove(eventpoll *ep, struct file *f) {
  spin_lock(&epoll_ctl_lock);
  epitem key = {.file = f};
  rb_node *node = rb_search(&ep->rbt, &key.rb_node, ep_cmp);
  if (!node) {
    spin_unlock(&epoll_ctl_lock);
    return -ENOENT;
  }
  epitem *epi = rb_entry(node, epitem, rb_node);
  ep_remove_item(epi);
  spin_unlock(&epoll_ctl_lock);
  file_put(f);
  kfree(epi);
  return 0;
}

static epitem *ep_find_fd_node(rb_node *node, struct files *owner, int fd) {
  if (!node)
    return NULL;
  epitem *found = ep_find_fd_node(node->rb_left, owner, fd);
  if (found)
    return found;
  epitem *epi = rb_entry(node, epitem, rb_node);
  if (epi->owner == owner && epi->fd == fd)
    return epi;
  return ep_find_fd_node(node->rb_right, owner, fd);
}

int ep_remove_fd(eventpoll *ep, struct files *owner, int fd) {
  spin_lock(&epoll_ctl_lock);
  epitem *epi = ep_find_fd_node(ep->rbt.rb_node, owner, fd);
  if (!epi) {
    spin_unlock(&epoll_ctl_lock);
    return -ENOENT;
  }
  struct file *f = epi->file;
  ep_remove_item(epi);
  spin_unlock(&epoll_ctl_lock);
  file_put(f);
  kfree(epi);
  return 0;
}

int ep_modify(eventpoll *ep, struct file *f, struct epoll_event *ev) {
  spin_lock(&epoll_ctl_lock);
  epitem key = {.file = f};
  rb_node *node = rb_search(&ep->rbt, &key.rb_node, ep_cmp);
  if (!node) {
    spin_unlock(&epoll_ctl_lock);
    return -ENOENT;
  }
  epitem *epi = rb_entry(node, epitem, rb_node);
  // Linux: EPOLLEXCLUSIVE may only be set at ADD time; MOD that flips the
  // exclusive state returns -EINVAL. Toggling ONESHOT/ET on MOD is allowed.
  int new_is_exclusive = !!(ev->events & EPOLLEXCLUSIVE);
  if (new_is_exclusive != epi->is_exclusive) {
    spin_unlock(&epoll_ctl_lock);
    return -EINVAL;
  }
  uint64_t ep_flags;
  spin_lock_irqsave(&ep->lock, &ep_flags);
  epi->events = ev->events & ~(EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE);
  epi->is_et = !!(ev->events & EPOLLET);
  epi->is_oneshot = !!(ev->events & EPOLLONESHOT);
  epi->is_disarmed = 0; // EPOLL_CTL_MOD re-arms a disarmed ONESHOT item
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
  spin_unlock_irqrestore(&ep->lock, ep_flags);
  spin_unlock(&epoll_ctl_lock);
  if (revents)
    __wake_up(&ep->wq, POLLIN);
  return 0;
}

void eventpoll_file_release(struct file *f) {
  while (1) {
    spin_lock(&epoll_ctl_lock);
    if (!f->epoll_items_initialized || list_empty(&f->epoll_items)) {
      spin_unlock(&epoll_ctl_lock);
      return;
    }

    epitem *epi = LIST_ENTRY(list_front(&f->epoll_items), epitem, file_node);
    eventpoll *ep = epi->ep;
    if (epi->target_wq)
      remove_wait_queue(epi->target_wq, &epi->wait);
    synchronize_rcu();
    uint64_t ep_flags;
    spin_lock_irqsave(&ep->lock, &ep_flags);
    if (epi->is_ready)
      list_remove(&epi->rdllist_node);
    rb_erase(&ep->rbt, &epi->rb_node);
    ep->nitems--;
    spin_unlock_irqrestore(&ep->lock, ep_flags);
    list_remove(&epi->file_node);
    spin_unlock(&epoll_ctl_lock);

    // Drop the interest's file reference only after it is unreachable from
    // both epoll and the file reverse list. The caller's file_put keeps f live.
    file_put(f);
    kfree(epi);
  }
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
  int fd = alloc_fd(proc->proc->files, 0);
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
  fd_install(proc->proc->files, fd, f);
  fd_set_cloexec(proc->proc->files, fd, (flags & EPOLL_CLOEXEC) ? 1 : 0);
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
    if (op == EPOLL_CTL_DEL) {
      // Some descriptors (notably SCM_RIGHTS device fds) still have aliases
      // in another process after this process closes its local number. Such a
      // file remains alive, so close cannot auto-detach it. Let a subsequent
      // DEL identify the original interest by its owning fd table and number.
      int ret = ep_remove_fd(ep, proc->proc->files, (int)fd);
      file_put(ef);
      return ret == -ENOENT ? -EBADF : ret;
    }
    file_put(ef);
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  // epfd == fd: registering an epoll fd onto itself would let the fd's
  // ep_poll_callback feed its own ready_list, forming a report→wake→report
  // loop. Linux rejects this with -EINVAL (after fd resolution, so bad fds
  // still get -EBADF). ef and f are the same struct file here, so file_get
  // bumped its refcount twice — balance with two file_put.
  if (ef == f) {
    file_put(f);
    file_put(ef);
    return -EINVAL;
  }

  int ret;
  if (op != EPOLL_CTL_DEL) {
    struct epoll_event ev;
    if (copy_from_user(&ev, (void *)ev_ptr, sizeof(ev))) {
      ret = -EFAULT;
    } else if (op == EPOLL_CTL_ADD) {
      ret = ep_insert(ep, f, proc->proc->files, (int)fd, &ev);
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
  wait.exclusive = 0; // epoll_wait waiters are shared — all must be woken
  list_init(&wait.node);
  add_wait_queue(&ep->wq, &wait);

  int n = 0;
  while (1) {
    // prepare_to_wait: mark BLOCKED before taking ep->lock and re-checking
    // ready_list, so a late ep_poll_callback __wake_up hits an already-BLOCKED
    // task.
    proc->state = BLOCKED;
    proc->wait_event = WAIT_POLL;
    proc->wait_timed_out = 0;
    uint64_t ep_flags;
    spin_lock_irqsave(&ep->lock, &ep_flags);
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
          // EPOLLONESHOT defers disarm until after the report succeeds below,
          // so it must NOT be re-enqueued here — once reported it is disarmed
          // until EPOLL_CTL_MOD re-arms it.
          if (!epi->is_oneshot) {
            list_push_back(&ep->ready_list, &epi->rdllist_node);
            epi->is_ready = 1;
          }
        }
        struct epoll_event ev = {.events = epi->revents,
                                 .data = {.u64 = epi->user_data}};
        if (copy_to_user(&((struct epoll_event *)ev_ptr)[n], &ev, sizeof(ev))) {
          spin_unlock_irqrestore(&ep->lock, ep_flags);
          sched_cancel_spurious_wake(proc);
          remove_wait_queue(&ep->wq, &wait);
          file_put(ef);
          return -EFAULT;
        }
        n++;
        // EPOLLONESHOT: the single report just happened — disarm so neither
        // this LT re-enqueue path nor ep_poll_callback can re-report until
        // EPOLL_CTL_MOD clears is_disarmed. is_ready is already 0 (removed
        // above and not re-enqueued for oneshot). For ET+ONESHOT this is
        // redundant-but-harmless (ET reports once anyway) and matches Linux.
        if (epi->is_oneshot)
          epi->is_disarmed = 1;
        if (it == pass_end)
          break; // reached end of this pass
        it = next;
      }
      spin_unlock_irqrestore(&ep->lock, ep_flags);
      // prepare_to_wait: the loop top marked BLOCKED; if a wake hit during
      // re-check and pushed run_node into the run_queue (state=READY), this
      // break without schedule() would leave a dangling run_node. Cancel the
      // spurious wake: drop run_node + reset state to RUNNING.
      sched_cancel_spurious_wake(proc);
      remove_wait_queue(&ep->wq, &wait);
      file_put(ef);
      return n;
    }
    spin_unlock_irqrestore(&ep->lock, ep_flags);

    if (timeout_ms == 0) {
      sched_cancel_spurious_wake(proc);
      remove_wait_queue(&ep->wq, &wait);
      file_put(ef);
      return 0;
    }

    // Block on WAIT_POLL
    uint64_t effective_deadline = deadline;
    uint64_t proc_alarm = 0;
    if (proc->proc && proc->proc->signal) {
      uint64_t sflags;
      spin_lock_irqsave(&proc->proc->signal->sig_lock, &sflags);
      proc_alarm = proc->proc->signal->alarm_deadline;
      spin_unlock_irqrestore(&proc->proc->signal->sig_lock, sflags);
    }
    if (effective_deadline == 0 && proc_alarm != 0) {
      // Indefinite wait but a process alarm is armed: use it as the wake
      // deadline so the timer queue can fire SIGALRM (otherwise the alarm
      // never triggers while blocked in epoll_wait).
      effective_deadline = proc_alarm;
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
      if (signal_pending(proc)) {
        sched_cancel_spurious_wake(proc);
        remove_wait_queue(&ep->wq, &wait);
        file_put(ef);
        return -ERESTART;
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
    // Accept any sigsetsize >= our 8-byte sigset_t (musl passes 128; we read
    // only the low 8 bytes = signals 1..64). The strict != check rejected
    // musl's 128 → EINVAL.
    if (sigsetsize < (int64_t)sizeof(sigset_t))
      return -EINVAL;
    if (copy_from_user(&new_mask, (void *)sigmask_ptr, sizeof(new_mask)))
      return -EFAULT;
    proc->proc->sig_blocked = new_mask;
    proc->proc->sig_blocked |= ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP)));
    have_mask = 1;
  }
  int64_t ret = sys_epoll_wait(epfd, ev_ptr, maxevents, timeout_ms);
  proc->proc->sig_blocked = old_blocked;
  (void)have_mask;
  return ret;
}
