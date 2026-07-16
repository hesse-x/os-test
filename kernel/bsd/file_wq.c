/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "kernel/bsd/eventpoll.h" // struct eventpoll (ep->wq)
#include "kernel/bsd/file_poll.h"
#include "kernel/bsd/netlink.h" // struct netlink_sock (wq field)
#include "kernel/bsd/pty.h"     // struct pty (wq field)
#include "kernel/bsd/socket.h"  // struct unix_sock (wq field)
#include "kernel/bsd/types.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/wait_queue.h"

// Lazily allocate a wait_queue_head and store it into *out_wq. Returns the wq,
// or NULL on OOM. Mirrors ep_wq_alloc in eventpoll.c.
static wait_queue_head *wq_alloc(wait_queue_head **out_wq) {
  wait_queue_head *wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
  if (!wq)
    return NULL;
  init_wait_queue_head(wq);
  *out_wq = wq;
  return wq;
}

// Resolve which wait_queue_head a file exposes for poll/epoll waiters, lazily
// allocating the per-type wq on first use. This MUST return the same wq the
// fd's data-ready path __wake_ups (pty->wq, sock->wq, pipe->close_wq,
// nlsock->wq, ep->wq, or the generic per-file f->wq used by eventfd/timerfd/
// signalfd/evdev broker consumers), otherwise a waiter registered here is
// never woken and (on return) the cleanup remove_wait_queue targets a wq that
// nobody else touches — leaking the stack wait node and corrupting that wq's
// list after stack reuse. sys_poll and sys_epoll_wait both resolve through
// here (ep_target_wq delegates to file_wq_get) so the two paths can't diverge.
wait_queue_head *file_wq_get(struct file *f) {
  /* epoll fd: ep->wq, woken by ep_poll_callback's __wake_up(&ep->wq). */
  if (f->type == FD_EPOLL && f->epoll) {
    return &f->epoll->wq;
  }
  /* pipe: p->wq (eager-allocated), woken by pipe close/read/write paths. */
  if (f->type == FD_PIPE && f->pipe) {
    return f->pipe->wq;
  }
  /* AF_UNIX socket: sock->wq, woken by sendmsg/recvmsg/shutdown. */
  if (f->type == FD_SOCKET && f->sock) {
    if (f->sock->wq)
      return f->sock->wq;
    return wq_alloc(&f->sock->wq);
  }
  /* pty/tty: pty->wq, woken by pty master/slave read/write. */
  if (f->type == FD_TTY && f->pty) {
    if (f->pty->wq)
      return f->pty->wq;
    return wq_alloc(&f->pty->wq);
  }
  /* AF_NETLINK: nlsock->wq, woken by nl_group_broadcast. */
  if (f->type == FD_NETLINK && f->nlsock) {
    if (f->nlsock->wq)
      return f->nlsock->wq;
    return wq_alloc(&f->nlsock->wq);
  }
  /* FD_IPC: per-file f->wq, woken by sys_req/notify/resp/msg_to/msg_resp
   * enqueue paths (evdev_refact.md §5.6). */
  if (f->type == FD_IPC) {
    if (f->wq)
      return f->wq;
    return wq_alloc(&f->wq);
  }
  /* Generic per-file wq (eventfd/timerfd/signalfd/other). */
  if (f->wq)
    return f->wq;
  return wq_alloc(&f->wq);
}
