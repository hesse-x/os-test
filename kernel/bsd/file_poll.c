/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>

#include "arch/x64/smp.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/eventfd.h"
#include "kernel/bsd/eventpoll.h"
#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/fops.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/netlink.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/pty.h"
#include "kernel/bsd/signal.h"
#include "kernel/bsd/signalfd.h"
#include "kernel/bsd/socket.h"
#include "kernel/bsd/timerfd.h"
#include "kernel/bsd/types.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"

#include <xos/socket.h>

struct drm_fence;
// 从 sys_poll 抽取的 per-type 就绪判断。纯查询，不阻塞、不加长锁。
// 语义与 socket.c sys_poll 的 if-else 链完全一致（无语义改变）。
__poll file_poll(struct file *f, __poll events) {
  __poll revents = 0;
  if (!f)
    return 0;

  if (f->f_op) {
    if (f->f_op->poll)
      return f->f_op->poll(current_task, f, events);
    return events & (POLLIN | POLLOUT);
  }

  if (f->type == FD_PIPE) {
    struct pipe *p = f->pipe;
    if (p) {
      // POLLIN: data available or EOF (no writers)
      if (p->head != p->tail || refcount_read(&p->p_count) <= 1) {
        if (events & POLLIN)
          revents |= POLLIN;
      }
      // POLLOUT: space available or peer closed
      if ((p->head + 1) % PIPE_BUF_SIZE != p->tail ||
          refcount_read(&p->p_count) <= 1) {
        if (events & POLLOUT)
          revents |= POLLOUT;
      }
    }
  } else if (f->type == FD_SOCKET) {
    struct unix_sock *sock = f->sock;
    if (sock) {
      spin_lock(&socket_lock);

      // POLLIN: data available or shutdown_read
      if (sock->recv_queue_head || sock->shutdown_read) {
        if (events & POLLIN)
          revents |= POLLIN;
      }

      // POLLOUT: not shutdown_write
      if (!sock->shutdown_write && sock->state == UNIX_CONNECTED) {
        if (events & POLLOUT)
          revents |= POLLOUT;
      }

      // POLLHUP: peer gone (always reported regardless of events mask)
      if (sock->state == UNIX_CLOSED ||
          (sock->peer >= 0 && sock->peer < MAX_PROC &&
           task_get(sock->peer)->pid != sock->peer)) {
        revents |= POLLHUP;
      }

      // For LISTEN socket: POLLIN = has pending connections
      if (sock->state == UNIX_LISTEN && sock->backlog_len > 0) {
        if (events & POLLIN)
          revents |= POLLIN;
      }

      spin_unlock(&socket_lock);
    }
  } else if (f->type == FD_FILE) {
    // FD_FILE: always writable (buffered), POLLIN if not at EOF
    if (events & POLLOUT)
      revents |= POLLOUT;
    // For POLLIN: check if offset < file_size
    if ((events & POLLIN) && f->file_data._offset < f->file_data.file_size) {
      revents |= POLLIN;
    }
  } else if (f->type == FD_TTY) {
    struct pty *pty = f->pty;
    if (pty) {
      int is_master = pty_is_master_inode(f->inode);
      revents |= pty_poll(pty, is_master, events);
    }
  } else if (f->type == FD_DEV) {
    // FD_DEV: call dev_ops.poll callback for kernel devices
    struct inode *ip = f->inode;
    if (ip && ip->i_priv) {
      struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
      if (ops->driver_pid == 0 && ops->poll) {
        revents |= ops->poll(current_task, events);
      }
    }
    // User-space driver: always ready (IPC handles events)
    if (f->target_pid > 0) {
      if (events & POLLIN)
        revents |= POLLIN;
      if (events & POLLOUT)
        revents |= POLLOUT;
    }
  } else if (f->type == FD_EPOLL) {
    struct eventpoll *ep = f->epoll;
    if (ep && !list_empty(&ep->ready_list))
      revents |= (events & POLLIN);
  } else if (f->type == FD_EVENTFD) {
    struct eventfd_ctx *ctx = f->eventfd;
    if (ctx) {
      if ((events & POLLIN) && ctx->count > 0)
        revents |= POLLIN;
      if ((events & POLLOUT) && ctx->count < EVENTFD_MAX)
        revents |= POLLOUT;
    }
  } else if (f->type == FD_TIMERFD) {
    struct timerfd_ctx *tfd = f->timerfd;
    if (tfd && tfd->ticks > 0)
      revents |= (events & POLLIN);
  } else if (f->type == FD_SIGNALFD) {
    struct signalfd_ctx *sfd = f->signalfd;
    if (sfd) {
      proc *bp = current_task->proc;
      if (bp) {
        uint64_t pend =
            __atomic_load_n(&bp->sig_pending, __ATOMIC_ACQUIRE) & sfd->sigmask;
        if (bp->signal)
          pend |= bp->signal->shared_pending & sfd->sigmask;
        if (pend & ~bp->sig_blocked)
          revents |= (events & POLLIN);
      }
    }
  } else if (f->type == FD_SYNC_FILE) {
    /* FD_SYNC_FILE (plan2): POLLIN once the bound fence is signaled. The
     * probe is a driver-layer accessor (drm_fence_is_signaled) so this BSD
     * file does not include the driver-layer drm_internal.h. */
    extern bool drm_fence_is_signaled(struct drm_fence * fence);
    struct drm_fence *fence = f->sync_file_fence;
    if (fence && drm_fence_is_signaled(fence))
      revents |= (events & POLLIN);
  } else if (f->type == FD_IPC) {
    // FD_IPC: POLLIN iff owner's recv queue is non-empty.  Non-owner
    // (cross-process hand-off) reports 0 — never ready, harmless to a
    // foreign epoll (evdev_refact.md §4.3 越权防御).
    pid_t owner_pid = f->ipcfd_owner_pid;
    if (owner_pid >= 0 && owner_pid < MAX_PROC) {
      xtask *o = task_get(owner_pid);
      if (o->pid == owner_pid && o == current_task &&
          o->recv_head != o->recv_tail)
        revents |= (events & POLLIN);
    }
  } else if (f->type == FD_NETLINK) {
    struct netlink_sock *nlsock = f->nlsock;
    if (nlsock) {
      spin_lock(&nl_group_lock);
      if (nlsock->recv_queue_head) {
        if (events & POLLIN)
          revents |= POLLIN;
      }
      spin_unlock(&nl_group_lock);
      // Netlink is always writable (broadcast does not block)
      if (events & POLLOUT)
        revents |= POLLOUT;
    }
  } else {
    // FD_SHM etc: always ready
    if (events & POLLIN)
      revents |= POLLIN;
    if (events & POLLOUT)
      revents |= POLLOUT;
  }
  return revents;
}
