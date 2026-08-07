/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/workqueue.h"

#include <stddef.h>

#include "arch/x64/apic.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/kthread.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"

struct workqueue {
  spinlock lock;
  list_node ready;
  list_node delayed;
  struct kthread *worker;
  struct work *current;
  bool accepting;
  uint32_t pending;
  completion idle;
  void *owner;
  const struct work_owner_ops *owner_ops;
  char name[32];
};

static void work_owner_get(struct workqueue *wq) {
  if (wq->owner_ops && wq->owner_ops->get)
    wq->owner_ops->get(wq->owner);
}

static void work_owner_put(struct workqueue *wq) {
  if (wq->owner_ops && wq->owner_ops->put)
    wq->owner_ops->put(wq->owner);
}

void init_work(struct work *work, void (*func)(struct work *)) {
  __memset(work, 0, sizeof(*work));
  list_init(&work->node);
  work->func = func;
  work->state = WORK_IDLE;
  init_completion(&work->finished);
}

void init_delayed_work(struct delayed_work *work, void (*func)(struct work *)) {
  __memset(work, 0, sizeof(*work));
  init_work(&work->work, func);
}

static void delayed_insert(struct workqueue *wq, struct delayed_work *delayed) {
  list_node *at = wq->delayed.next;
  while (at != &wq->delayed) {
    struct work *other_work = LIST_ENTRY(at, struct work, node);
    struct delayed_work *other =
        LIST_ENTRY(other_work, struct delayed_work, work);
    if (delayed->deadline_ns < other->deadline_ns)
      break;
    at = at->next;
  }
  delayed->work.node.prev = at->prev;
  delayed->work.node.next = at;
  at->prev->next = &delayed->work.node;
  at->prev = &delayed->work.node;
}

static int workqueue_thread(void *arg) {
  struct workqueue *wq = (struct workqueue *)arg;
  while (!wq->worker)
    schedule();

  for (;;) {
    struct work *work = NULL;
    uint64_t deadline = 0;
    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    uint64_t now = sched_clock();
    while (!list_empty(&wq->delayed)) {
      struct work *first =
          LIST_ENTRY(list_front(&wq->delayed), struct work, node);
      struct delayed_work *delayed =
          LIST_ENTRY(first, struct delayed_work, work);
      if (delayed->deadline_ns > now) {
        deadline = delayed->deadline_ns;
        break;
      }
      list_remove(&first->node);
      list_init(&first->node);
      first->delayed = false;
      list_push_back(&wq->ready, &first->node);
    }
    if (!list_empty(&wq->ready)) {
      work = LIST_ENTRY(list_front(&wq->ready), struct work, node);
      list_remove(&work->node);
      list_init(&work->node);
      work->state = WORK_RUNNING;
      wq->current = work;
    } else if (kthread_should_stop(wq->worker)) {
      ASSERT(wq->pending == 0);
      spin_unlock_irqrestore(&wq->lock, flags);
      break;
    }
    spin_unlock_irqrestore(&wq->lock, flags);

    if (!work) {
      kthread_wait_timeout(wq->worker, deadline);
      continue;
    }

    work->func(work);

    spin_lock_irqsave(&wq->lock, &flags);
    ASSERT(work->state == WORK_RUNNING || work->state == WORK_CANCELING);
    work->state = WORK_IDLE;
    work->wq = NULL;
    wq->current = NULL;
    ASSERT(wq->pending > 0);
    wq->pending--;
    bool idle = wq->pending == 0;
    spin_unlock_irqrestore(&wq->lock, flags);
    complete_all(&work->finished);
    if (idle)
      complete_all(&wq->idle);
    work_owner_put(wq);
  }
  return 0;
}

struct workqueue *alloc_ordered_workqueue(const char *name, void *owner,
                                          const struct work_owner_ops *ops) {
  struct workqueue *wq = kmalloc(sizeof(*wq));
  if (!wq)
    return NULL;
  __memset(wq, 0, sizeof(*wq));
  wq->lock = SPINLOCK_INIT;
  list_init(&wq->ready);
  list_init(&wq->delayed);
  init_completion(&wq->idle);
  complete_all(&wq->idle);
  wq->accepting = true;
  wq->owner = owner;
  wq->owner_ops = ops;
  if (name) {
    int i = 0;
    for (; name[i] && i < (int)sizeof(wq->name) - 1; i++)
      wq->name[i] = name[i];
    wq->name[i] = '\0';
  }
  wq->worker = kthread_create(workqueue_thread, wq, wq->name);
  if (!wq->worker) {
    kfree(wq);
    return NULL;
  }
  return wq;
}

static bool queue_work_common(struct workqueue *wq, struct work *work,
                              uint64_t deadline_ns, bool delayed) {
  if (!wq || !work || !work->func)
    return false;
  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  if (!wq->accepting || work->state != WORK_IDLE) {
    spin_unlock_irqrestore(&wq->lock, flags);
    return false;
  }
  if (wq->pending++ == 0)
    reinit_completion(&wq->idle);
  reinit_completion(&work->finished);
  work->wq = wq;
  work->state = WORK_QUEUED;
  work->delayed = delayed;
  work_owner_get(wq);
  if (delayed) {
    struct delayed_work *dw = LIST_ENTRY(work, struct delayed_work, work);
    dw->deadline_ns = deadline_ns;
    delayed_insert(wq, dw);
  } else {
    list_push_back(&wq->ready, &work->node);
  }
  spin_unlock_irqrestore(&wq->lock, flags);
  kthread_wake(wq->worker);
  return true;
}

bool queue_work(struct workqueue *wq, struct work *work) {
  return queue_work_common(wq, work, 0, false);
}

bool queue_delayed_work(struct workqueue *wq, struct delayed_work *work,
                        uint64_t delay_ns) {
  uint64_t now = sched_clock();
  uint64_t deadline = UINT64_MAX - now < delay_ns ? UINT64_MAX : now + delay_ns;
  return queue_work_common(wq, &work->work, deadline, true);
}

bool cancel_work_sync(struct work *work) {
  if (!work || !work->wq)
    return false;
  struct workqueue *wq = work->wq;
  ASSERT(current_task != kthread_task(wq->worker));
  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  if (work->state == WORK_QUEUED) {
    list_remove(&work->node);
    list_init(&work->node);
    work->state = WORK_IDLE;
    work->wq = NULL;
    ASSERT(wq->pending > 0);
    wq->pending--;
    bool idle = wq->pending == 0;
    spin_unlock_irqrestore(&wq->lock, flags);
    complete_all(&work->finished);
    if (idle)
      complete_all(&wq->idle);
    work_owner_put(wq);
    return true;
  }
  if (work->state == WORK_RUNNING)
    work->state = WORK_CANCELING;
  bool wait = work->state == WORK_CANCELING;
  spin_unlock_irqrestore(&wq->lock, flags);
  if (wait)
    wait_for_completion_timeout(&work->finished, UINT64_MAX);
  return wait;
}

void flush_workqueue(struct workqueue *wq) {
  if (!wq)
    return;
  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  bool idle = wq->pending == 0;
  spin_unlock_irqrestore(&wq->lock, flags);
  if (!idle)
    wait_for_completion_timeout(&wq->idle, UINT64_MAX);
}

void destroy_workqueue(struct workqueue *wq) {
  if (!wq)
    return;
  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  wq->accepting = false;
  spin_unlock_irqrestore(&wq->lock, flags);
  flush_workqueue(wq);
  kthread_stop(wq->worker);
  kfree(wq);
}
