/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ipcfd: evdev's downstream-IPC fd.  An FD_IPC fd bound to the creator's own
// recv queue, made pollable + readable so evdev's main loop can epoll_wait on
// (irqfd, ipcfd) and split the HID-interrupt and downstream-IPC sources (see
// evdev_refact.md §3.3/§4.3).  read(ipcfd) is the dequeue primitive (calls
// ipc_dequeue); it does NOT call sys_recv.  Owner-checked against pid reuse:
// the fd stores ipcfd_owner_pid (not a raw xtask*); read/poll resolve the
// owner via task_get(pid) + pid-alive check + ==current_task, so a reaped
// owner or a cross-process fd hand-off cannot UAF or dequeue a foreign queue.

#include <stddef.h>
#include <stdint.h>

#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/ipcfd.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h" // ipc_dequeue, copy_from_user/copy_to_user
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"

#include <xos/errno.h>

// Resolve the owner xtask for an FD_IPC fd, with pid-alive + caller-is-owner
// checks.  Returns NULL if the owner is reaped (pid reused) or the caller is
// not the owner (cross-process hand-off via SCM_RIGHTS).  Uses the
// "task_get(pid) + pid field check" idiom, same as sys_resp.
static xtask *ipcfd_owner(struct file *f) {
  pid_t owner_pid = f->ipcfd_owner_pid;
  if (owner_pid < 0 || owner_pid >= MAX_PROC)
    return NULL;
  xtask *o = task_get(owner_pid);
  if (o->pid != owner_pid) // reaped / slot reused
    return NULL;
  if (o != current_task) // not the owner
    return NULL;
  return o;
}

int64_t sys_ipcfd_create(void) {
  xtask *proc = current_task;
  spin_lock(&proc->proc->files->fd_lock);
  int fd = alloc_fd(proc->proc->files, 3);
  if (fd < 0) {
    spin_unlock(&proc->proc->files->fd_lock);
    return -EMFILE;
  }
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(&proc->proc->files->fd_lock);
    return -ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_IPC;
  f->ipcfd_owner_pid = proc->pid;
  // Publish the fd's file to the task so enqueue paths can find + wake its
  // wq.  Hold a reference: the close path drops it.  (evdev_refact.md §5.6)
  file_get(f);
  proc->ipcfd_file = f;
  fd_install(proc->proc->files, fd, f);
  spin_unlock(&proc->proc->files->fd_lock);
  return fd;
}

// Non-blocking read = dequeue from the owner's recv queue.  Owner-checked:
// -EPERM if caller is not the owner; -EAGAIN if the queue is empty.  Mirrors
// sys_recv's per-type handling via the shared ipc_dequeue (Stage 3).
int64_t ipcfd_do_read(struct file *f, void __user *buf, void __user *data_buf,
                      size_t data_buf_len) {
  xtask *owner = ipcfd_owner(f);
  if (!owner)
    return -EPERM;
  return ipc_dequeue(owner, buf, data_buf, data_buf_len);
}

// sys_ipcfd_read: the FD_IPC read syscall.  read(fd, buf, count) is only
// 3-arg, but ipcfd needs the recv_msg target + variable-length payload
// (data_buf + len) — the same shape as recv().  Resolves the fd, owner-checks
// via ipcfd_do_read, and returns its result verbatim (0 / -EAGAIN / -EPERM /
// -EINVAL / -EFAULT).  (evdev_refact.md §4.3)
int64_t sys_ipcfd_read(int64_t fd, int64_t buf, int64_t data_buf,
                       int64_t data_buf_len) {
  if (fd < 0 || fd >= MAX_FD)
    return -EBADF;

  xtask *proc = current_task;
  rcu_read_lock();
  struct file *f = fd_lookup(proc->proc->files, (int)fd);
  if (!f || f->type != FD_IPC) {
    rcu_read_unlock();
    return -EBADF;
  }
  file_get(f);
  rcu_read_unlock();

  int64_t ret =
      ipcfd_do_read(f, (void __user *__force)buf,
                    (void __user *__force)data_buf, (size_t)data_buf_len);
  file_put(f);
  return ret;
}

// ipcfd_close: called from file_put's FD_IPC teardown.  Clears the owner
// task's ipcfd_file back-link and drops the reference taken in
// sys_ipcfd_create.  After this, enqueue paths see NULL and skip the ipcfd
// wake (the fd is going away).  Idempotent: a NULL owner means already
// cleared.  (evdev_refact.md §4.3 生命周期 / §5.6)
void ipcfd_close(struct file *f) {
  pid_t owner_pid = f->ipcfd_owner_pid;
  if (owner_pid < 0 || owner_pid >= MAX_PROC)
    return;
  xtask *o = task_get(owner_pid);
  if (o->pid != owner_pid)
    return; // owner reaped; the reference will be dropped by file_put
  if (o->ipcfd_file == f) {
    o->ipcfd_file = NULL;
    file_put(f); // drop the create-time reference
  }
}
