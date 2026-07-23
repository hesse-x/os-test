/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// POSIX (fcntl) record locks — S09.
//
// per-inode lock list (inode->i_flock) + conflict resolution. Only FD_REGULAR
// files may carry locks; pipe/socket/dev/shm/dir get -ENOLCK. F_SETLKW blocks
// on the inode's shared wait queue (inode->wq, the same ringbuf-backed wq
// epoll/poll use) and is signal-interruptible.
//
// Scope (deliberately NOT in this file, tracked in doc/design/todo.md):
//   - OFD locks (F_OFD_*)  → placeholder -EINVAL (sys_fcntl side).
//   - F_SETOWN/F_SETSIG actual SIGIO delivery → only stored, no AIO path.
//   - per-pid reverse lock index → linear inode scan on exit (inode count
//   small).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x64/smp.h"
#include "kernel/bsd/file_lock.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/signal.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"

#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/signal.h>
#include <xos/syscall_nums.h>

// A single POSIX byte-range lock. Lives on inode->i_flock.
struct file_lock {
  list_node node;  // link into inode->i_flock
  pid_t owner_pid; // lock holder (POSIX: locks are per-process)
  int type;        // F_RDLCK / F_WRLCK
  uint64_t start;  // absolute byte start (resolved from l_whence)
  uint64_t end;    // absolute byte end (exclusive); UINT64_MAX = to EOF
};

// EOF is represented as an unbounded end. l_len==0 means "to end of file".
#define FLOCK_END_EOF UINT64_MAX

static uint64_t flock_end(uint64_t start, uint64_t len) {
  if (len == 0)
    return FLOCK_END_EOF;
  return start + len;
}

// Same-process locks never conflict (POSIX: a process may re-lock/extend its
// own ranges). Read-read never conflicts. Conflict otherwise needs an
// overlapping range with at least one writer. (Kept inline in find_conflict.)

// Find a lock on ip that conflicts with a (type, [start,end)) lock owned by
// pid. Caller holds ip->i_flock_lock. Returns NULL if none.
static struct file_lock *find_conflict(struct inode *ip, pid_t pid, int type,
                                       uint64_t start, uint64_t end) {
  for (list_node *n = ip->i_flock.next; n != &ip->i_flock; n = n->next) {
    struct file_lock *fl = (struct file_lock *)n;
    if (fl->owner_pid == pid)
      continue;
    if (fl->type == F_RDLCK && type == F_RDLCK)
      continue;
    if (fl->start < end && start < fl->end)
      return fl;
  }
  return NULL;
}

// Remove and free every lock owned by pid that overlaps [start,end), splitting
// the boundaries of partially-overlapping locks so only the requested range is
// released (Linux posix_locks_unlock semantics). Caller holds i_flock_lock.
static void locks_delete_range(struct inode *ip, pid_t pid, uint64_t start,
                               uint64_t end) {
  list_node *n = ip->i_flock.next;
  while (n != &ip->i_flock) {
    struct file_lock *fl = (struct file_lock *)n;
    n = n->next;

    if (fl->owner_pid != pid)
      continue;
    // No overlap?
    if (fl->end <= start || end <= fl->start)
      continue;

    uint64_t fl_start = fl->start;
    uint64_t fl_end = fl->end;
    int fl_type = fl->type;
    list_remove(&fl->node);
    kfree(fl);

    // Re-insert preserved residuals (front and/or back), as new locks.
    if (fl_start < start) {
      struct file_lock *front =
          (struct file_lock *)kmalloc(sizeof(struct file_lock));
      if (front) {
        front->owner_pid = pid;
        front->type = fl_type;
        front->start = fl_start;
        front->end = start;
        list_push_back(&ip->i_flock, &front->node);
      }
    }
    if (end < fl_end) {
      struct file_lock *back =
          (struct file_lock *)kmalloc(sizeof(struct file_lock));
      if (back) {
        back->owner_pid = pid;
        back->type = fl_type;
        back->start = end;
        back->end = fl_end;
        list_push_back(&ip->i_flock, &back->node);
      }
    }
  }
}

// Insert a new lock for pid. POSIX "replace" semantics on same-pid overlap are
// simplified: delete any overlapping same-pid ranges first, then insert. This
// matches Linux for the common cases (re-lock, upgrade/downgrade within owned
// ranges). Caller holds i_flock_lock.
static int locks_insert(struct inode *ip, pid_t pid, int type, uint64_t start,
                        uint64_t end) {
  // Remove same-pid locks overlapping the new range (replace semantics).
  locks_delete_range(ip, pid, start, end);

  struct file_lock *fl = (struct file_lock *)kmalloc(sizeof(struct file_lock));
  if (!fl)
    return -ENOMEM;
  fl->owner_pid = pid;
  fl->type = type;
  fl->start = start;
  fl->end = end;
  list_push_back(&ip->i_flock, &fl->node);
  return 0;
}

// wq callback for F_SETLKW: __wake_up(inode->wq) wakes every blocked locker,
// each re-checks its own conflict under i_flock_lock.
static void flock_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *target = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(target);
}

// Lazily allocate inode->wq (the shared ringbuf-backed wait queue). F_SETLKW
// is the first user of inode->wq for regular files; epoll/poll on regular
// files also route here so a single wake reaches both.
static wait_queue_head *inode_wq_get(struct inode *ip) {
  wait_queue_head *wq = ip->wq;
  if (wq)
    return wq;
  wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
  if (!wq)
    return NULL;
  init_wait_queue_head(wq);
  ip->wq = wq;
  return wq;
}

// True if the current task has an unblocked signal pending (interrupts an
// indefinite F_SETLKW). Mirrors the pipe-write check.
static bool task_signal_pending(xtask *proc) {
  if (!proc->proc)
    return false;
  uint64_t pend = __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
  uint64_t deliv = pend & ~proc->proc->sig_blocked;
  deliv |= (pend & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
  return deliv != 0;
}

// Apply a lock (F_RDLCK/F_WRLCK) or unlock (F_UNLCK) for pid over [start,end).
// blocking=1 makes a conflicting F_SETLKW wait on inode->wq (signal-interrupt).
static int64_t apply_lock(struct inode *ip, pid_t pid, int type, uint64_t start,
                          uint64_t end, int blocking) {
  wait_queue_head *wq = NULL;

  for (;;) {
    spin_lock(&ip->i_flock_lock);
    if (type == F_UNLCK) {
      locks_delete_range(ip, pid, start, end);
      spin_unlock(&ip->i_flock_lock);
      // Wake any blocked SETLKW waiters — a released range may satisfy them.
      if (ip->wq)
        __wake_up(ip->wq, 0);
      return 0;
    }
    struct file_lock *conf = find_conflict(ip, pid, type, start, end);
    if (!conf) {
      int r = locks_insert(ip, pid, type, start, end);
      spin_unlock(&ip->i_flock_lock);
      if (ip->wq)
        __wake_up(ip->wq, 0); // wake SETLKW waiters that may now succeed
      return r;
    }
    // Conflict.
    spin_unlock(&ip->i_flock_lock);
    if (!blocking)
      return -EAGAIN;

    // Block on inode->wq until the conflict clears or a signal arrives.
    if (!wq)
      wq = inode_wq_get(ip);
    if (!wq)
      return -ENOMEM; // cannot block without a wq

    xtask *proc = current_task;
    wait_queue_t wait;
    wait.func = flock_wake_cb;
    wait.data = proc;
    wait.exclusive = 0;
    list_init(&wait.node);
    add_wait_queue(wq, &wait);
    proc->state = BLOCKED;
    proc->wait_event = WAIT_NONE;
    /* No user timeout for F_SETLKW (POSIX: indefinite, signal-interruptible
     * only). Borrow the process alarm deadline (if armed) so a pending SIGALRM
     * can interrupt, mirroring sys_pause / blocking pipe write. */
    uint64_t alarm_dl = 0;
    if (proc->proc && proc->proc->signal) {
      uint64_t sflags;
      spin_lock_irqsave(&proc->proc->signal->sig_lock, &sflags);
      alarm_dl = proc->proc->signal->alarm_deadline;
      spin_unlock_irqrestore(&proc->proc->signal->sig_lock, sflags);
    }
    if (alarm_dl != 0) {
      proc->wait_deadline = alarm_dl;
      int cpu = proc->assigned_cpu;
      uint64_t flags;
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
      sched_timer_queue_insert(cpu, proc);
      spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
    } else {
      proc->wait_deadline = 0;
    }
    schedule();
    sched_cancel_spurious_wake(proc);
    remove_wait_queue(wq, &wait);
    if (task_signal_pending(proc))
      return -EINTR;
    // loop: re-check conflict under i_flock_lock
  }
}

int64_t do_fcntl_lock(xtask *proc, struct file *f, int cmd, struct flock *lk) {
  if (!f->inode)
    return -ENOLCK;

  /* 1. Validate l_type / l_whence. */
  if (lk->l_type != F_RDLCK && lk->l_type != F_WRLCK && lk->l_type != F_UNLCK)
    return -EINVAL;
  if (lk->l_whence != SEEK_SET && lk->l_whence != SEEK_CUR &&
      lk->l_whence != SEEK_END)
    return -EINVAL;

  /* 2. Resolve absolute byte range. */
  uint64_t base;
  switch (lk->l_whence) {
  case SEEK_SET:
    base = 0;
    break;
  case SEEK_CUR:
    base = f->offset;
    break;
  case SEEK_END:
    base = f->inode->size;
    break;
  default:
    return -EINVAL;
  }
  /* l_start/l_len are signed; negative start/len is allowed (Linux) but this OS
   * has no real file-lock users exercising it, so reject underflow simply. */
  if (lk->l_start < 0)
    return -EINVAL;
  uint64_t start = base + (uint64_t)lk->l_start;
  uint64_t end = flock_end(start, (uint64_t)lk->l_len);
  if (end < start)
    return -EINVAL;

  struct inode *ip = f->inode;
  pid_t pid = proc->pid;

  /* 3. F_GETLK: probe (never blocks). */
  if (cmd == F_GETLK) {
    spin_lock(&ip->i_flock_lock);
    struct file_lock *conf = find_conflict(ip, pid, lk->l_type, start, end);
    if (conf) {
      lk->l_type = (short)conf->type;
      lk->l_whence = SEEK_SET;
      lk->l_start = (long)conf->start;
      lk->l_len =
          (conf->end == FLOCK_END_EOF) ? 0 : (long)(conf->end - conf->start);
      lk->l_pid = conf->owner_pid;
    } else {
      lk->l_type = F_UNLCK;
      lk->l_whence = SEEK_SET;
      lk->l_start = 0;
      lk->l_len = 0;
      lk->l_pid = 0;
    }
    spin_unlock(&ip->i_flock_lock);
    return 0;
  }

  /* 4. F_SETLK / F_SETLKW. */
  int blocking = (cmd == F_SETLKW) ? 1 : 0;
  return apply_lock(ip, pid, lk->l_type, start, end, blocking);
}

// --- process-exit & inode-eviction cleanup ---

static void release_pid_on_inode(struct inode *ip, void *ctx) {
  pid_t dead_pid = (pid_t)(uintptr_t)ctx;
  spin_lock(&ip->i_flock_lock);
  list_node *n = ip->i_flock.next;
  bool changed = false;
  while (n != &ip->i_flock) {
    struct file_lock *fl = (struct file_lock *)n;
    n = n->next;
    if (fl->owner_pid == dead_pid) {
      list_remove(&fl->node);
      kfree(fl);
      changed = true;
    }
  }
  spin_unlock(&ip->i_flock_lock);
  if (changed && ip->wq)
    __wake_up(ip->wq, 0);
}

void file_lock_release_pid(pid_t dead_pid) {
  inode_for_each(release_pid_on_inode, (void *)(uintptr_t)dead_pid);
}

void file_lock_release_all(struct inode *ip) {
  spin_lock(&ip->i_flock_lock);
  list_node *n = ip->i_flock.next;
  while (n != &ip->i_flock) {
    struct file_lock *fl = (struct file_lock *)n;
    n = n->next;
    list_remove(&fl->node);
    kfree(fl);
  }
  spin_unlock(&ip->i_flock_lock);
}
