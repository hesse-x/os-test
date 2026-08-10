/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/selftest.h"

#include <stddef.h>
#include <xos/errno.h>
#include <xos/page.h>

#include "arch/x64/utils.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/completion.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/kthread.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/vma.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/workqueue.h"

#ifdef TEST
static struct work *selftest_first;
static struct work *selftest_second;
static struct work *selftest_third;
static int selftest_order;
static atomic_t selftest_owner_refs;
static atomic_t selftest_vma_refs;

#define SELFTEST_CHECK(cond) BUG_ON(!(cond))

static void selftest_work(struct work *work) {
  int expected = work == selftest_first ? 0 : work == selftest_second ? 1 : 2;
  SELFTEST_CHECK(selftest_order == expected);
  selftest_order++;
}

static void selftest_owner_get(void *owner) { atomic_inc((atomic_t *)owner); }

static void selftest_owner_put(void *owner) { atomic_dec((atomic_t *)owner); }

static void selftest_vma_owner_lifecycle(void) {
  atomic_set(&selftest_vma_refs, 0);
  const struct vma_owner_ops owner_ops = {
      .get = selftest_owner_get,
      .put = selftest_owner_put,
  };
  mm address_space = {0};
  mmap_region *front = kmalloc(sizeof(*front));
  SELFTEST_CHECK(front);
  __memset(front, 0, sizeof(*front));
  front->vaddr = 0x100000;
  front->size = 2 * PAGE_SIZE;
  front->fd = -1;
  SELFTEST_CHECK(vma_attach_owner(front, &selftest_vma_refs, &owner_ops) == 0);
  SELFTEST_CHECK(vma_insert_sorted(&address_space, front) == 0);
  mmap_region *tail =
      vma_split(&address_space, front, front->vaddr + PAGE_SIZE, PAGE_SIZE);
  SELFTEST_CHECK(tail);
  SELFTEST_CHECK(atomic_read(&selftest_vma_refs) == 2);
  vma_owner_put(front);
  vma_owner_put(tail);
  kfree(front);
  kfree(tail);
  SELFTEST_CHECK(atomic_read(&selftest_vma_refs) == 0);
}

static void selftest_vma_shm_split_refs(void) {
  mm address_space = {0};
  shm *backing = kmalloc(sizeof(*backing));
  mmap_region *front = kmalloc(sizeof(*front));
  SELFTEST_CHECK(backing && front);
  __memset(backing, 0, sizeof(*backing));
  __memset(front, 0, sizeof(*front));
  refcount_set(&backing->s_count, 1); // Owned by the initial region.
  front->vaddr = 0x200000;
  front->size = 3 * PAGE_SIZE;
  front->fd = -1;
  front->shm_obj = backing;
  SELFTEST_CHECK(vma_insert_sorted(&address_space, front) == 0);

  mmap_region *mid =
      vma_split(&address_space, front, front->vaddr + PAGE_SIZE, PAGE_SIZE);
  SELFTEST_CHECK(mid && mid->next);
  mmap_region *tail = mid->next;
  SELFTEST_CHECK(refcount_read(&backing->s_count) == 3);

  shm_put(front->shm_obj);
  shm_put(mid->shm_obj);
  shm_put(tail->shm_obj);
  kfree(front);
  kfree(mid);
  kfree(tail);
}

static int async_selftest_thread(void *arg) {
  (void)arg;
  completion c;
  init_completion(&c);
  complete(&c);
  SELFTEST_CHECK(wait_for_completion_timeout(&c, 1000000ULL) == 0);
  SELFTEST_CHECK(wait_for_completion_timeout(&c, 1000000ULL) == -ETIMEDOUT);
  selftest_vma_owner_lifecycle();
  selftest_vma_shm_split_refs();

  atomic_set(&selftest_owner_refs, 0);
  const struct work_owner_ops owner_ops = {
      .get = selftest_owner_get,
      .put = selftest_owner_put,
  };
  struct workqueue *wq = alloc_ordered_workqueue(
      "xcore-test-wq", &selftest_owner_refs, &owner_ops);
  SELFTEST_CHECK(wq);
  struct work first, second;
  struct delayed_work third, canceled;
  init_work(&first, selftest_work);
  init_work(&second, selftest_work);
  init_delayed_work(&third, selftest_work);
  init_delayed_work(&canceled, selftest_work);
  selftest_first = &first;
  selftest_second = &second;
  selftest_third = &third.work;
  selftest_order = 0;
  SELFTEST_CHECK(queue_work(wq, &first));
  SELFTEST_CHECK(!queue_work(wq, &first));
  SELFTEST_CHECK(queue_work(wq, &second));
  SELFTEST_CHECK(queue_delayed_work(wq, &third, 2000000ULL));
  SELFTEST_CHECK(queue_delayed_work(wq, &canceled, 1000000000ULL));
  SELFTEST_CHECK(cancel_work_sync(&canceled.work));
  flush_workqueue(wq);
  SELFTEST_CHECK(selftest_order == 3);
  destroy_workqueue(wq);
  SELFTEST_CHECK(atomic_read(&selftest_owner_refs) == 0);
  printk(LOG_INFO, "xcore async selftest: PASS\n");
  return 0;
}
#endif

void xcore_async_selftest_start(void) {
#ifdef TEST
  SELFTEST_CHECK(
      kthread_run_detached(async_selftest_thread, NULL, "xcore-selftest") == 0);
#endif
}
