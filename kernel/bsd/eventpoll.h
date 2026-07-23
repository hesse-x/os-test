/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_EVENTPOLL_H
#define KERNEL_BSD_EVENTPOLL_H

#include <stdint.h>

#include "kernel/bsd/poll_types.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/rbtree.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"

#define EP_MAX_ITEMS 128

struct file;
struct epoll_event;

typedef struct epitem {
  rb_node rb_node;        // in eventpoll.rbt (ordered by file*)
  list_node rdllist_node; // in eventpoll.ready_list
  wait_queue_t wait;      // on monitored file/sock/pipe wq
  struct file *file;      // monitored fd's file (held via file_get)
  __poll events;          // user-registered event mask (excludes mode flags)
  __poll revents;         // current ready events
  uint64_t user_data;     // epoll_event.data.u64
  int is_ready;           // currently on ready_list
  int is_et;              // EPOLLET mode flag
  int is_oneshot;         // EPOLLONESHOT mode flag (set on ADD/MOD)
  int is_disarmed;        // ONESHOT: reported once, disarmed until MOD re-arms
  int is_exclusive;       // EPOLLEXCLUSIVE: unicast wake (one exclusive waiter)
  struct eventpoll *ep;   // back-pointer
} epitem;

typedef struct eventpoll {
  spinlock lock;        // protects rbt + ready_list
  wait_queue_head wq;   // epoll_wait blocks here
  rb_root rbt;          // interest list
  list_node ready_list; // ready epitems
  int nitems;           // current interest-list size
} eventpoll;

eventpoll *eventpoll_create(void);
void eventpoll_release(eventpoll *ep);
int ep_insert(eventpoll *ep, struct file *f, struct epoll_event *ev);
int ep_remove(eventpoll *ep, struct file *f);
int ep_modify(eventpoll *ep, struct file *f, struct epoll_event *ev);

int64_t sys_epoll_create(int64_t size);
int64_t sys_epoll_create1(int64_t flags);
int64_t sys_epoll_ctl(int64_t epfd, int64_t op, int64_t fd, int64_t ev_ptr);
int64_t sys_epoll_wait(int64_t epfd, int64_t ev_ptr, int64_t maxevents,
                       int64_t timeout_ms);
int64_t sys_epoll_pwait(int64_t epfd, int64_t ev_ptr, int64_t maxevents,
                        int64_t timeout_ms, int64_t sigmask_ptr,
                        int64_t sigsetsize);

#endif // KERNEL_BSD_EVENTPOLL_H
