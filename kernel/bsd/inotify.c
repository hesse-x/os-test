/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/bsd/inotify.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/mount.h" // ERR_PTR/IS_ERR/PTR_ERR
#include "kernel/bsd/proc.h"
#include "kernel/bsd/types.h"
#include "kernel/bsd/vfs.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/kasan.h" // copy_*/strncpy_from_user
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/rbtree.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"

#include "kernel/bsd/kfcntl.h"
#include <xos/errno.h>
#include <xos/socket.h> // POLLIN

// ===================== global inode→watch index =====================
// To avoid invading struct inode with a per-inode watch list, all watches are
// hung off a global rbtree keyed by inode pointer. A trigger point looks up its
// inode here; a miss is O(1) and costs nothing for the un-watched majority.
// Protected by g_inotify_index_lock; the trigger side wraps the lookup+enqueue
// in rcu_read_lock (see §6.3) so close's synchronize_rcu waits out readers
// before freeing an instance.

typedef struct inotify_inode_entry {
  rb_node rb; // node in g_inotify_inode_index, key = inode pointer
  struct inode
      *inode;        // key (not refcounted; entry removed when last watch goes)
  list_node watches; // head of inotify_watch.inode_node list on this inode
} inotify_inode_entry;

static rb_root g_inotify_inode_index = RB_ROOT;
static spinlock g_inotify_index_lock = SPINLOCK_INIT;

static int inotify_index_cmp(rb_node *a, rb_node *b) {
  inotify_inode_entry *ea = rb_entry(a, inotify_inode_entry, rb);
  inotify_inode_entry *eb = rb_entry(b, inotify_inode_entry, rb);
  // Compare by inode pointer value.
  if (ea->inode < eb->inode)
    return -1;
  if (ea->inode > eb->inode)
    return 1;
  return 0;
}

// Find (or NULL) the index entry for `inode`. Caller must hold
// g_inotify_index_lock (or be in an RCU read-side section that only reads the
// node fields — but the list mutation under the entry needs the lock, so all
// callers take the lock).
static inotify_inode_entry *inotify_index_lookup(struct inode *inode) {
  inotify_inode_entry key;
  key.inode = inode;
  rb_node *n = rb_search(&g_inotify_inode_index, &key.rb, inotify_index_cmp);
  return n ? rb_entry(n, inotify_inode_entry, rb) : NULL;
}

// Get-or-create the index entry for `inode`. Returns NULL on OOM. On success
// the entry's watch list is ready for list_push_back; caller holds
// g_inotify_index_lock.
static inotify_inode_entry *inotify_index_get(struct inode *inode) {
  inotify_inode_entry *e = inotify_index_lookup(inode);
  if (e)
    return e;
  e = (inotify_inode_entry *)kmalloc(sizeof(inotify_inode_entry));
  if (!e)
    return NULL;
  e->inode = inode;
  list_init(&e->watches);
  rb_insert(&g_inotify_inode_index, &e->rb, inotify_index_cmp);
  return e;
}

// Remove the index entry iff its watch list is now empty. Caller holds
// g_inotify_index_lock.
static void inotify_index_maybe_drop(inotify_inode_entry *e) {
  if (list_empty(&e->watches)) {
    rb_erase(&g_inotify_inode_index, &e->rb);
    kfree(e);
  }
}

// ===================== helpers =====================

// Wake a blocked inotify read waiter (registered on inst->wq).
static void inotify_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *proc = (xtask *)wq->data;
  (void)flags;
  wake_wq_target(proc);
}

// Find a watch by wd in this instance. Caller holds inst->lock.
static inotify_watch *inotify_find_wd(inotify *in, int wd) {
  for (list_node *n = in->watches.next; n != &in->watches; n = n->next) {
    inotify_watch *w = LIST_ENTRY(n, inotify_watch, inst_node);
    if (w->wd == wd)
      return w;
  }
  return NULL;
}

// Find a watch by inode in this instance. Caller holds inst->lock.
static inotify_watch *inotify_find_inode(inotify *in, struct inode *inode) {
  for (list_node *n = in->watches.next; n != &in->watches; n = n->next) {
    inotify_watch *w = LIST_ENTRY(n, inotify_watch, inst_node);
    if (w->inode == inode)
      return w;
  }
  return NULL;
}

// Allocate a fresh wd, skipping any wd already in use. Caller holds inst->lock.
static int inotify_alloc_wd(inotify *in) {
  int wd = in->next_wd;
  for (int tries = 0; tries < INOTIFY_MAX_WATCHES + 1; tries++) {
    if (wd <= 0)
      wd = 1; // wd must be >= 1 (0 is reserved/invalid)
    if (!inotify_find_wd(in, wd)) {
      in->next_wd = wd + 1;
      return wd;
    }
    wd++;
  }
  return -1; // exhausted (shouldn't happen below INOTIFY_MAX_WATCHES)
}

// Resolve the user pathname to an inode (+1 ref, caller puts). Returns the
// inode or an ERR_PTR(-errno). Mirrors resolve_path_or_fd in syscall.c but
// lives here so inotify is self-contained (absolute path via vfs_open_kern,
// relative path from CWD/root).
static struct inode *inotify_resolve_path(const char *upath) {
  char kpath[256];
  long n = strncpy_from_user(kpath, upath, sizeof(kpath));
  if (n < 0)
    return ERR_PTR(-EFAULT);
  if (n >= (long)sizeof(kpath))
    return ERR_PTR(-ENAMETOOLONG);
  if (kpath[0] == '\0')
    return ERR_PTR(-ENOENT);

  if (kpath[0] == '/') {
    struct inode *ip = vfs_open_kern(kpath); // +1 or NULL
    if (!ip)
      return ERR_PTR(-ENOENT);
    return ip;
  }
  // Relative: resolve from CWD (AT_FDCWD → root, no per-process CWD). +1.
  struct inode *start = resolve_dirfd_start(AT_FDCWD);
  if (IS_ERR(start))
    return start;
  struct inode *ip = path_walk_from(start, kpath); // +1 or NULL
  inode_put(start);
  if (!ip)
    return ERR_PTR(-ENOENT);
  return ip;
}

// ===================== sys_inotify_init1 =====================
int64_t sys_inotify_init1(int64_t flags) {
  if ((uint32_t)flags & ~(IN_CLOEXEC | IN_NONBLOCK))
    return -EINVAL;

  inotify *in = (inotify *)kmalloc(sizeof(inotify));
  if (!in)
    return -ENOMEM;
  in->lock = SPINLOCK_INIT;
  list_init(&in->watches);
  list_init(&in->event_queue);
  in->next_wd = 1;
  in->nqueued = 0;
  in->dropped = 0;
  init_wait_queue_head(&in->wq); // embedded wq, pre-initialized

  xtask *proc = current_task;
  spin_lock(&proc->proc->files->fd_lock);
  int fd = alloc_fd(proc->proc->files, 0);
  if (fd < 0) {
    spin_unlock(&proc->proc->files->fd_lock);
    kfree(in);
    return -EMFILE;
  }
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(&proc->proc->files->fd_lock);
    kfree(in);
    return -ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_INOTIFY;
  f->f_op = NULL;
  f->private_data = in; // wq is embedded in `in`, f->wq stays NULL
  if (flags & IN_NONBLOCK)
    f->flags |= O_NONBLOCK;
  fd_install(proc->proc->files, fd, f);
  fd_set_cloexec(proc->proc->files, fd, (flags & IN_CLOEXEC) ? 1 : 0);
  spin_unlock(&proc->proc->files->fd_lock);
  return fd;
}

// Validate fd is an inotify fd of the current process; return the instance.
// RCU-protected fd lookup like every fd-table reader; bumps no ref (the fd
// holds the file alive for the syscall's duration since the caller is the
// owner and won't close it mid-call).
static inotify *inotify_fd_to_inst(int64_t fd) {
  if (fd < 0 || fd >= MAX_FD)
    return NULL;
  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, (int)fd);
  inotify *in = NULL;
  if (f && f->type == FD_INOTIFY)
    in = (inotify *)f->private_data;
  rcu_read_unlock();
  return in;
}

// ===================== sys_inotify_add_watch =====================
int64_t sys_inotify_add_watch(int64_t fd, int64_t pathname, int64_t mask) {
  // mask must carry at least one legal event bit and no reserved bits.
  uint32_t m = (uint32_t)mask;
  const uint32_t legal_events = IN_ALL_EVENTS;
  const uint32_t legal_flags = IN_ONESHOT | IN_ONLYDIR | IN_DONT_FOLLOW |
                               IN_EXCL_UNLINK | IN_MASK_CREATE | IN_MASK_ADD;
  if ((m & (legal_events | legal_flags)) == 0)
    return -EINVAL;
  // Reserved/undefined high bits (outside events + flags + IN_ISDIR) rejected.
  // IN_ISDIR is an out-only flag (set by the kernel in events, never in mask);
  // reject it in the input mask.
  if (m & ~(legal_events | legal_flags))
    return -EINVAL;

  inotify *in = inotify_fd_to_inst(fd);
  if (!in)
    return -EBADF;

  struct inode *ip = inotify_resolve_path((const char *)pathname);
  if (IS_ERR(ip))
    return (int64_t)PTR_ERR(ip);
  // ip is +1.

  spin_lock(&in->lock);

  // Existing watch on the same inode. Linux semantics: default (no flag) =
  // REPLACE the event mask; IN_MASK_ADD = OR it in; IN_MASK_CREATE = fail
  // (handled above). Behavior flags (IN_ONESHOT/IN_ONLYDIR/...) are OR'd in
  // either way (inotify.md §4.2; P1 refines per-flag).
  inotify_watch *w = inotify_find_inode(in, ip);
  if (w) {
    if (m & IN_MASK_CREATE) {
      // IN_MASK_CREATE: fail if a watch already exists on this inode.
      spin_unlock(&in->lock);
      inode_put(ip);
      return -EEXIST;
    }
    if (m & IN_MASK_ADD)
      w->mask |= (m & legal_events);
    else
      w->mask = (w->mask & ~legal_events) | (m & legal_events);
    w->mask |= (m & legal_flags);
    int wd = w->wd;
    spin_unlock(&in->lock);
    inode_put(ip); // balance the +1 we took (existing watch holds its own ref)
    return wd;
  }

  // Cap watch count.
  int watch_count = 0;
  for (list_node *n = in->watches.next; n != &in->watches; n = n->next)
    watch_count++;
  if (watch_count >= INOTIFY_MAX_WATCHES) {
    spin_unlock(&in->lock);
    inode_put(ip);
    return -ENOSPC;
  }

  int wd = inotify_alloc_wd(in);
  if (wd < 0) {
    spin_unlock(&in->lock);
    inode_put(ip);
    return -ENOSPC;
  }

  w = (inotify_watch *)kmalloc(sizeof(inotify_watch));
  if (!w) {
    spin_unlock(&in->lock);
    inode_put(ip);
    return -ENOMEM;
  }
  w->inst = in;
  w->inode = ip; // watch takes ownership of the +1 ref
  w->mask = m;
  w->wd = wd;
  list_push_back(&in->watches, &w->inst_node);

  // Insert into the global inode index.
  spin_lock(&g_inotify_index_lock);
  inotify_inode_entry *e = inotify_index_get(ip);
  if (!e) {
    spin_unlock(&g_inotify_index_lock);
    // Roll back: detach from inst list and free.
    list_remove(&w->inst_node);
    spin_unlock(&in->lock);
    inode_put(ip);
    kfree(w);
    return -ENOMEM;
  }
  list_push_back(&e->watches, &w->inode_node);
  spin_unlock(&g_inotify_index_lock);

  spin_unlock(&in->lock);
  return wd;
}

// ===================== sys_inotify_rm_watch =====================
int64_t sys_inotify_rm_watch(int64_t fd, int64_t wd) {
  if (wd <= 0)
    return -EINVAL;

  inotify *in = inotify_fd_to_inst(fd);
  if (!in)
    return -EBADF;

  spin_lock(&in->lock);
  inotify_watch *w = inotify_find_wd(in, (int)wd);
  if (!w) {
    spin_unlock(&in->lock);
    return -EINVAL;
  }
  // Detach from the instance list under inst->lock.
  list_remove(&w->inst_node);
  struct inode *ip = w->inode;
  spin_unlock(&in->lock);

  // Detach from the global index under the index lock.
  spin_lock(&g_inotify_index_lock);
  inotify_inode_entry *e = inotify_index_lookup(ip);
  if (e) {
    list_remove(&w->inode_node);
    inotify_index_maybe_drop(e);
  }
  spin_unlock(&g_inotify_index_lock);

  // Release outside all locks (inode_put may trigger inode reclaim callbacks).
  inode_put(ip);
  kfree(w);
  return 0;
}

// ===================== inotify_inode_event (VFS trigger) =====================
// Lock order: g_inotify_index_lock → inst->lock. Acquired only here; NEVER
// called with any fstype lock (fat_lock/i_lock) held — trigger points call
// after releasing those (inotify.md §4.4/§6.2). The rcu_read_lock wrapping
// protects the inst pointer from concurrent close freeing it; close's
// synchronize_rcu (in inotify_release) waits out this whole section.
void inotify_inode_event(struct inode *inode, uint32_t mask, uint32_t cookie,
                         const char *name) {
  BUG_ON(!inode);

  // Truncate name to NAME_MAX; compute len.
  uint32_t namelen = 0;
  if (name) {
    while (name[namelen] && namelen <= INOTIFY_NAME_MAX)
      namelen++;
    if (namelen > INOTIFY_NAME_MAX)
      namelen = INOTIFY_NAME_MAX;
  }

  rcu_read_lock();
  spin_lock(&g_inotify_index_lock);
  inotify_inode_entry *e = inotify_index_lookup(inode);
  if (!e) {
    // Un-watched inode: O(1) miss, nothing to do.
    spin_unlock(&g_inotify_index_lock);
    rcu_read_unlock();
    return;
  }
  // Iterate a snapshot-safe copy of the watch list: we hold the index lock so
  // no watch can be added/removed from this inode's list concurrently, and
  // rcu_read_lock keeps inst alive across close.
  list_node *next;
  for (list_node *n = e->watches.next; n != &e->watches; n = next) {
    next = n->next;
    inotify_watch *w = LIST_ENTRY(n, inotify_watch, inode_node);
    if (!(w->mask & mask))
      continue; // watch not interested in this event

    inotify *in = w->inst;
    uint64_t flags;
    spin_lock_irqsave(&in->lock, &flags);
    if (in->nqueued < INOTIFY_MAX_EVENTS) {
      // Linux reports len including the terminating NUL and padding. Keeping
      // each record 4-byte aligned also lets userspace safely walk a batch.
      uint32_t event_len = name ? (namelen + 1 + 3) & ~3u : 0;
      inotify_event_node *node =
          (inotify_event_node *)kmalloc(sizeof(inotify_event_node) + event_len);
      if (node) {
        node->wd = w->wd;
        node->mask = mask;
        node->cookie = cookie;
        node->len = event_len;
        if (event_len)
          __memset(node->name, 0, event_len);
        if (namelen)
          __memcpy(node->name, name, namelen);
        list_push_back(&in->event_queue, &node->node);
        in->nqueued++;
      } else {
        in->dropped++;
      }
    } else {
      in->dropped++; // queue full: drop newest (P0; P1 emits IN_Q_OVERFLOW)
    }
    spin_unlock_irqrestore(&in->lock, flags);

    // Wake waiters. wq is embedded in `in`; rcu_read_lock keeps `in` alive.
    __wake_up(&in->wq, POLLIN);
  }
  spin_unlock(&g_inotify_index_lock);
  rcu_read_unlock();
}

// ===================== inotify_do_read =====================
// The kernel can't include musl's <sys/inotify.h>; this matches its layout
// exactly (int + 3× uint32_t = 16 bytes, name[] follows). Used only for sizing.
struct inotify_event_min {
  int wd;
  uint32_t mask;
  uint32_t cookie;
  uint32_t len;
};
_Static_assert(sizeof(struct inotify_event_min) == 16,
               "inotify_event header must be 16 bytes (musl UAPI ABI)");

// Blocking read of one or more struct inotify_event records. Returns total
// bytes copied, or negative errno. Multiple events are concatenated as long as
// the user buffer holds a whole next event; a partial next event stays queued.
int64_t inotify_do_read(struct xtask *proc, struct file *f, void *buf,
                        size_t count) {
  (void)proc;
  inotify *in = (inotify *)f->private_data;
  if (!in)
    return -EBADF;
  if (count < sizeof(struct inotify_event_min))
    return -EINVAL;
  if (count > INOTIFY_MAX_READ)
    count = INOTIFY_MAX_READ;

  wait_queue_head *wq = &in->wq; // embedded, always valid
  wait_queue_t wait;
  wait.func = inotify_wake_cb;
  wait.data = current_task;
  wait.exclusive = 0;
  list_init(&wait.node);
  add_wait_queue(wq, &wait);

  int64_t ret;
  size_t copied = 0;
  for (;;) {
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_POLL;
    current_task->wait_timed_out = 0;

    uint64_t flags;
    spin_lock_irqsave(&in->lock, &flags);
    // Drain events that fit wholly in the remaining buffer.
    while (!list_empty(&in->event_queue) && copied < count) {
      list_node *first = in->event_queue.next;
      inotify_event_node *node = LIST_ENTRY(first, inotify_event_node, node);
      size_t evsz = sizeof(struct inotify_event_min) + node->len;
      if (copied + evsz > count)
        break; // next event doesn't fit; leave it queued for next read

      // Build the on-wire struct inotify_event. Layout matches musl
      // <sys/inotify.h>: { int wd; uint32_t mask, cookie, len; char name[]; }.
      struct {
        int wd;
        uint32_t mask;
        uint32_t cookie;
        uint32_t len;
      } hdr = {node->wd, node->mask, node->cookie, node->len};

      // Copy header + name to user. copy_to_user of two segments; on failure
      // stop without rolling back already-copied events (Linux partial-read).
      size_t off = copied;
      if (copy_to_user((char *)buf + off, &hdr, sizeof(hdr)) != 0) {
        if (copied == 0) {
          // Nothing copied yet and the first header failed → -EFAULT.
          list_remove(first);
          kfree(node);
          in->nqueued--;
          spin_unlock_irqrestore(&in->lock, flags);
          ret = -EFAULT;
          goto out;
        }
        break; // keep node queued; return what we have
      }
      if (node->len && copy_to_user((char *)buf + off + sizeof(hdr), node->name,
                                    node->len) != 0) {
        if (copied == 0) {
          list_remove(first);
          kfree(node);
          in->nqueued--;
          spin_unlock_irqrestore(&in->lock, flags);
          ret = -EFAULT;
          goto out;
        }
        break;
      }

      list_remove(first);
      kfree(node);
      in->nqueued--;
      copied += evsz;
    }
    if (copied > 0) {
      spin_unlock_irqrestore(&in->lock, flags);
      ret = (int64_t)copied;
      goto out;
    }
    spin_unlock_irqrestore(&in->lock, flags);

    if (f->flags & O_NONBLOCK) {
      ret = -EAGAIN;
      goto out;
    }
    if (signal_pending(current_task)) {
      ret = -ERESTART;
      goto out;
    }
    schedule();
  }

out:
  sched_cancel_spurious_wake(current_task);
  remove_wait_queue(wq, &wait);
  return ret;
}

// ===================== inotify_poll =====================
uint32_t inotify_poll(struct file *f, uint32_t events) {
  inotify *in = (inotify *)f->private_data;
  if (!in)
    return 0;
  uint64_t flags;
  spin_lock_irqsave(&in->lock, &flags);
  uint32_t revents = (in->nqueued > 0) ? (events & POLLIN) : 0;
  spin_unlock_irqrestore(&in->lock, flags);
  return revents;
}

// ===================== inotify_release (close) =====================
// Order is load-bearing (inotify.md §6.3):
//   1. detach all watches from the global index (after this, no new trigger
//      can reach this instance).
//   2. synchronize_rcu() — wait out trigger-side readers already holding `in`
//      under rcu_read_lock (inotify_inode_event).
//   3. only then free watches/inode refs/queue/instance (lock-external).
void inotify_release(struct file *f) {
  inotify *in = (inotify *)f->private_data;
  if (!in)
    return;
  f->private_data = NULL;

  // 1. Detach every watch from the global index.
  spin_lock(&g_inotify_index_lock);
  list_node *next;
  for (list_node *n = in->watches.next; n != &in->watches; n = next) {
    next = n->next;
    inotify_watch *w = LIST_ENTRY(n, inotify_watch, inst_node);
    inotify_inode_entry *e = inotify_index_lookup(w->inode);
    if (e) {
      list_remove(&w->inode_node);
      inotify_index_maybe_drop(e);
    }
  }
  spin_unlock(&g_inotify_index_lock);

  // 2. Wait for trigger-side RCU readers that already hold `in` to exit.
  synchronize_rcu();

  // 3. Free everything (no lock held — inode_put/kfree may recurse).
  while (!list_empty(&in->watches)) {
    list_node *n = in->watches.next;
    inotify_watch *w = LIST_ENTRY(n, inotify_watch, inst_node);
    list_remove(n);
    inode_put(w->inode);
    kfree(w);
  }
  while (!list_empty(&in->event_queue)) {
    list_node *n = in->event_queue.next;
    inotify_event_node *node = LIST_ENTRY(n, inotify_event_node, node);
    list_remove(n);
    kfree(node);
  }
  // wq is embedded — freed with `in`. f->wq is NULL for FD_INOTIFY, so the
  // generic file_put kfree(f->wq) is a no-op (no double-free).
  kfree(in);
}
