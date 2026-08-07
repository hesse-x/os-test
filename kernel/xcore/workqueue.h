/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_XCORE_WORKQUEUE_H
#define KERNEL_XCORE_WORKQUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/xcore/completion.h"
#include "kernel/xcore/list.h"

struct workqueue;

enum work_state { WORK_IDLE = 0, WORK_QUEUED, WORK_RUNNING, WORK_CANCELING };

struct work {
  list_node node;
  void (*func)(struct work *work);
  struct workqueue *wq;
  enum work_state state;
  bool delayed;
  completion finished;
};

struct delayed_work {
  struct work work;
  uint64_t deadline_ns;
};

struct work_owner_ops {
  void (*get)(void *owner);
  void (*put)(void *owner);
};

void init_work(struct work *work, void (*func)(struct work *));
void init_delayed_work(struct delayed_work *work, void (*func)(struct work *));
struct workqueue *alloc_ordered_workqueue(const char *name, void *owner,
                                          const struct work_owner_ops *ops);
bool queue_work(struct workqueue *wq, struct work *work);
bool queue_delayed_work(struct workqueue *wq, struct delayed_work *work,
                        uint64_t delay_ns);
bool cancel_work_sync(struct work *work);
void flush_workqueue(struct workqueue *wq);
void destroy_workqueue(struct workqueue *wq);

#endif
