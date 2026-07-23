/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_WAIT_QUEUE_H
#define KERNEL_WAIT_QUEUE_H

#include "kernel/xcore/list.h"
#include "kernel/xcore/spinlock.h"

typedef struct wait_queue_head {
  spinlock lock;
  list_node head; // 挂 wait_queue_t.node
} wait_queue_head;

#define WAIT_QUEUE_HEAD_INIT(name)                                             \
  {                                                                            \
    SPINLOCK_INIT, { &(name).head, &(name).head }                              \
  }

// 纯同步原语：func 的 flags 是调用方透传的盲数据，xcore 不解释其含义。
// poll/epoll 的事件掩码语义由 bsd 层回调自行解释（见
// file_poll.h/eventpoll.c）。
struct wait_queue_t;
typedef void (*wait_queue_func_t)(struct wait_queue_t *wq, unsigned long flags);

typedef struct wait_queue_t {
  list_node node;
  wait_queue_func_t func;
  void *data;    // 通常指向 epitem
  int exclusive; // 1 = exclusive waiter：__wake_up 唤醒一个即跳过其余
                 // exclusive waiter（非 exclusive 全唤醒），防惊群。
                 // 栈上 wait 节点须显式初始化（0 或 1），勿留栈残值。
} wait_queue_t;

void init_wait_queue_head(wait_queue_head *wq);
void add_wait_queue(wait_queue_head *wq, wait_queue_t *wait);
void remove_wait_queue(wait_queue_head *wq, wait_queue_t *wait);
void __wake_up(wait_queue_head *wq, unsigned long flags);

#endif // KERNEL_WAIT_QUEUE_H
