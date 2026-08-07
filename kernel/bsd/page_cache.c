/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"
#include <xos/errno.h>
#include <xos/page.h>

#define RA_MAX_PAGES PAGE_CACHE_RA_MAX_PAGES

static struct cache_page *page_cache_hash[1 << PAGE_CACHE_HASH_BITS];
static spinlock page_cache_lock = SPINLOCK_INIT;
static struct cache_page lru_head;
static struct cache_page lru_tail;
static bool lru_inited;
static struct cache_page *free_list;
static int free_count;
static struct page_cache_stats cache_stats;

enum {
  RA_LIFECYCLE_NONE = 0,
  RA_LIFECYCLE_OUTSTANDING = 1,
  RA_LIFECYCLE_RESOLVED = 2,
};

static enum page_cache_ra_bucket ra_bucket(uint32_t pages) {
  switch (pages) {
  case 1:
    return PAGE_CACHE_RA_BUCKET_1;
  case 4:
    return PAGE_CACHE_RA_BUCKET_4;
  case 8:
    return PAGE_CACHE_RA_BUCKET_8;
  case 16:
    return PAGE_CACHE_RA_BUCKET_16;
  default:
    return PAGE_CACHE_RA_BUCKET_OTHER;
  }
}

uint32_t page_cache_ra_cap(uint32_t requested_pages) {
  uint32_t cap = XOS_RA_MAX_PAGES ? XOS_RA_MAX_PAGES : 1;
  if (!requested_pages)
    requested_pages = 1;
  if (requested_pages > RA_MAX_PAGES)
    requested_pages = RA_MAX_PAGES;
  return requested_pages < cap ? requested_pages : cap;
}

static unsigned page_cache_hashfn(struct inode *ip, uint64_t page_index) {
  uintptr_t key = (uintptr_t)ip;
  key ^= key >> 9;
  key ^= (uintptr_t)page_index ^ (uintptr_t)(page_index >> 32);
  return (unsigned)key & ((1 << PAGE_CACHE_HASH_BITS) - 1);
}

static void lru_init(void) {
  lru_head.lru_next = &lru_tail;
  lru_tail.lru_prev = &lru_head;
  lru_head.lru_prev = NULL;
  lru_tail.lru_next = NULL;
  lru_inited = true;
}

static void lru_remove(struct cache_page *cp) {
  if (cp->lru_prev)
    cp->lru_prev->lru_next = cp->lru_next;
  if (cp->lru_next)
    cp->lru_next->lru_prev = cp->lru_prev;
  cp->lru_prev = NULL;
  cp->lru_next = NULL;
}

static void lru_insert_head(struct cache_page *cp) {
  cp->lru_next = lru_head.lru_next;
  cp->lru_prev = &lru_head;
  lru_head.lru_next->lru_prev = cp;
  lru_head.lru_next = cp;
}

static void lru_touch(struct cache_page *cp) {
  lru_remove(cp);
  lru_insert_head(cp);
}

static struct cache_page *free_list_pop(void) {
  struct cache_page *cp = free_list;
  if (!cp)
    return NULL;
  free_list = cp->hash_next;
  cp->hash_next = NULL;
  free_count--;
  return cp;
}

static void free_list_push(struct cache_page *cp) {
  cp->hash_next = free_list;
  free_list = cp;
  free_count++;
}

static void ra_resolve_locked(struct cache_page *cp, bool invalidation) {
  if (cp->ra_lifecycle != RA_LIFECYCLE_OUTSTANDING)
    return;
  unsigned source = cp->ra_source;
  if (source >= PAGE_CACHE_RA_SOURCE_COUNT)
    return;
  if (cache_stats.outstanding[source])
    cache_stats.outstanding[source]--;
  if (invalidation)
    cache_stats.invalidation_waste[source]++;
  else
    cache_stats.eviction_waste[source]++;
  cache_stats.readahead_waste++;
  cp->ra_lifecycle = RA_LIFECYCLE_RESOLVED;
  cp->flags &= ~CACHE_PAGE_READAHEAD;
}

static void ra_hit_locked(struct cache_page *cp) {
  if (cp->ra_lifecycle != RA_LIFECYCLE_OUTSTANDING)
    return;
  unsigned source = cp->ra_source;
  if (source >= PAGE_CACHE_RA_SOURCE_COUNT)
    return;
  if (cache_stats.outstanding[source])
    cache_stats.outstanding[source]--;
  cache_stats.hits[source]++;
  cache_stats.readahead_hits++;
  cp->ra_lifecycle = RA_LIFECYCLE_RESOLVED;
  cp->flags &= ~CACHE_PAGE_READAHEAD;
}

static void page_wait_wake(wait_queue_t *wait, unsigned long flags) {
  (void)flags;
  wake_wq_target((xtask *)wait->data);
}

static uint64_t page_cache_prepare_wait(xtask *task) {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));
  int cpu = task->assigned_cpu;
  spin_lock(&cpu_locals[cpu].scheduler_lock);
  task->state = BLOCKED;
  task->wait_event = WAIT_NONE;
  spin_unlock(&cpu_locals[cpu].scheduler_lock);
  return flags;
}

static void page_cache_finish_wait(uint64_t flags) {
  __asm__ volatile("pushq %0; popfq" : : "r"(flags));
}

static void page_cache_wait(struct cache_page *cp, uint32_t mask) {
  if (!current_task) {
    while (__atomic_load_n(&cp->flags, __ATOMIC_ACQUIRE) & mask)
      __asm__ volatile("pause");
    return;
  }
  wait_queue_t wait = {
      .func = page_wait_wake, .data = current_task, .exclusive = 0};
  list_init(&wait.node);
  add_wait_queue(&cp->waiters, &wait);
  for (;;) {
    uint64_t flags = page_cache_prepare_wait(current_task);
    if (!(__atomic_load_n(&cp->flags, __ATOMIC_ACQUIRE) & mask)) {
      sched_cancel_spurious_wake(current_task);
      page_cache_finish_wait(flags);
      break;
    }
    schedule();
    page_cache_finish_wait(flags);
  }
  remove_wait_queue(&cp->waiters, &wait);
}

void page_cache_init(void) {
  if (!lru_inited)
    lru_init();
  for (int i = 0; i < (1 << PAGE_CACHE_HASH_BITS); i++)
    page_cache_hash[i] = NULL;
  for (int i = 0; i < PAGE_CACHE_SIZE; i++) {
    struct cache_page *cp = kmalloc(sizeof(*cp));
    if (!cp)
      break;
    __memset(cp, 0, sizeof(*cp));
    init_wait_queue_head(&cp->waiters);
    free_list_push(cp);
  }
  printk(LOG_INFO, "page_cache_init: %d cache_page structs pre-allocated\n",
         free_count);
}

struct cache_page *page_cache_lookup(struct inode *ip, uint64_t page_index) {
  if (!ip)
    return NULL;
  unsigned bucket = page_cache_hashfn(ip, page_index);
  spin_lock(&page_cache_lock);
  for (struct cache_page *cp = page_cache_hash[bucket]; cp;
       cp = cp->hash_next) {
    if (cp->inode != ip || cp->page_index != page_index || !cp->data)
      continue;
    atomic_inc(&cp->pin_count);
    lru_touch(cp);
    uint32_t flags = cp->flags;
    spin_unlock(&page_cache_lock);
    if (flags & CACHE_PAGE_FILLING)
      page_cache_wait(cp, CACHE_PAGE_FILLING);
    flags = __atomic_load_n(&cp->flags, __ATOMIC_ACQUIRE);
    if (flags & (CACHE_PAGE_ERROR | CACHE_PAGE_INVALID)) {
      page_cache_release(cp);
      return NULL;
    }
    spin_lock(&page_cache_lock);
    ra_hit_locked(cp);
    spin_unlock(&page_cache_lock);
    return cp;
  }
  spin_unlock(&page_cache_lock);
  return NULL;
}

static int page_cache_evict_locked(void) {
  for (struct cache_page *cp = lru_tail.lru_prev; cp != &lru_head;
       cp = cp->lru_prev) {
    if (atomic_read(&cp->pin_count) != 0 || !cp->data ||
        (cp->flags &
         (CACHE_PAGE_FILLING | CACHE_PAGE_WRITEBACK | CACHE_PAGE_DIRTY)))
      continue;
    ra_resolve_locked(cp, false);
    unsigned bucket = page_cache_hashfn(cp->inode, cp->page_index);
    struct cache_page **link = &page_cache_hash[bucket];
    while (*link && *link != cp)
      link = &(*link)->hash_next;
    if (*link == cp)
      *link = cp->hash_next;
    lru_remove(cp);
    kfree(cp->data);
    cp->data = NULL;
    cp->inode = NULL;
    cp->flags = 0;
    free_list_push(cp);
    return 0;
  }
  return -ENOMEM;
}

static struct cache_page *
page_cache_alloc_locked(struct inode *ip, uint64_t page_index, bool readahead) {
  struct cache_page *cp = free_list_pop();
  if (!cp) {
    if (page_cache_evict_locked())
      return NULL;
    cp = free_list_pop();
  }
  cp->data = kmalloc(PAGE_SIZE);
  if (!cp->data) {
    free_list_push(cp);
    return NULL;
  }
  cp->inode = ip;
  cp->page_index = page_index;
  atomic_set(&cp->pin_count, 1);
  cp->flags = CACHE_PAGE_FILLING | (readahead ? CACHE_PAGE_READAHEAD : 0);
  cp->error = 0;
  cp->dirty_seq = 0;
  cp->ra_source = PAGE_CACHE_RA_SOURCE_COUNT;
  cp->ra_bucket = PAGE_CACHE_RA_BUCKET_OTHER;
  cp->ra_lifecycle = RA_LIFECYCLE_NONE;
  cp->generation++;
  unsigned bucket = page_cache_hashfn(ip, page_index);
  cp->hash_next = page_cache_hash[bucket];
  page_cache_hash[bucket] = cp;
  lru_insert_head(cp);
  return cp;
}

static void page_cache_unhash_locked(struct cache_page *cp) {
  unsigned bucket = page_cache_hashfn(cp->inode, cp->page_index);
  struct cache_page **link = &page_cache_hash[bucket];
  while (*link && *link != cp)
    link = &(*link)->hash_next;
  if (*link == cp)
    *link = cp->hash_next;
  cp->hash_next = NULL;
  lru_remove(cp);
  cp->flags |= CACHE_PAGE_INVALID;
}

static struct cache_page *find_locked(struct inode *ip, uint64_t index) {
  unsigned bucket = page_cache_hashfn(ip, index);
  for (struct cache_page *cp = page_cache_hash[bucket]; cp; cp = cp->hash_next)
    if (cp->inode == ip && cp->page_index == index && cp->data)
      return cp;
  return NULL;
}

int page_cache_get_ra(struct inode *ip, uint64_t page_index,
                      uint32_t window_pages, enum page_cache_ra_source source,
                      struct cache_page **out) {
  if (!ip || !out || !window_pages || source >= PAGE_CACHE_RA_SOURCE_COUNT)
    return -EINVAL;
  *out = page_cache_lookup(ip, page_index);
  if (*out)
    return 0;
  if (!ip->i_aop || !ip->i_aop->readpage)
    return -EIO;

  uint32_t requested = window_pages;
  window_pages = page_cache_ra_cap(window_pages);
  if (ip->type == INODE_REGULAR) {
    uint64_t file_pages = (ip->size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (page_index >= file_pages)
      window_pages = 1;
    else if (window_pages > file_pages - page_index)
      window_pages = (uint32_t)(file_pages - page_index);
  } else {
    window_pages = 1;
  }
  if (requested > 1) {
    cache_stats.calls[source]++;
    cache_stats.requested_pages[source] += requested;
    cache_stats.requested_window[source][ra_bucket(requested)]++;
    cache_stats.effective_window[source][ra_bucket(window_pages)]++;
  }

  struct cache_page *pages[RA_MAX_PAGES] = {0};
  uint32_t allocated = 0;
  spin_lock(&page_cache_lock);
  for (uint32_t i = 0; i < window_pages; i++) {
    uint64_t index = page_index + i;
    if (find_locked(ip, index)) {
      if (i == 0)
        cache_stats.reservation_conflicts[source]++;
      break;
    }
    pages[allocated] = page_cache_alloc_locked(ip, index, i != 0);
    if (!pages[allocated])
      break;
    allocated++;
  }
  spin_unlock(&page_cache_lock);
  if (!allocated)
    return page_cache_get_ra(ip, page_index, 1, source, out);

  uint32_t admitted = 0;
  int demand_error = 0;
  for (uint32_t i = 0; i < allocated; i++) {
    __memset(pages[i]->data, 0, PAGE_SIZE);
    int rc = ip->i_aop->readpage(ip, pages[i]->page_index, pages[i]->data);
    spin_lock(&page_cache_lock);
    pages[i]->flags &= ~CACHE_PAGE_FILLING;
    if (rc) {
      pages[i]->error = rc;
      pages[i]->flags |= CACHE_PAGE_ERROR;
      page_cache_unhash_locked(pages[i]);
      if (i == 0)
        demand_error = rc;
    } else {
      pages[i]->flags |= CACHE_PAGE_UPTODATE;
      admitted++;
      if (i != 0) {
        pages[i]->ra_source = (uint8_t)source;
        pages[i]->ra_bucket = (uint8_t)ra_bucket(allocated);
        pages[i]->ra_lifecycle = RA_LIFECYCLE_OUTSTANDING;
        cache_stats.admitted_speculative[source]++;
        cache_stats.outstanding[source]++;
        if (cache_stats.outstanding[source] >
            cache_stats.outstanding_peak[source])
          cache_stats.outstanding_peak[source] =
              cache_stats.outstanding[source];
      }
    }
    spin_unlock(&page_cache_lock);
    __wake_up(&pages[i]->waiters, 0);
  }

  for (uint32_t i = demand_error ? 0 : 1; i < allocated; i++)
    page_cache_release(pages[i]);
  if (demand_error) {
    page_cache_release(pages[0]);
    return demand_error;
  }
  cache_stats.admitted_demand[source]++;
  cache_stats.admitted_window[source][ra_bucket(admitted)]++;
  if (admitted > 1) {
    cache_stats.readahead_batches++;
    cache_stats.readahead_pages += admitted - 1;
    cache_stats.batch_io_commands[source] += admitted;
  }
  *out = pages[0];
  return 0;
}

struct cache_page *page_cache_fill(struct inode *ip, uint64_t page_index) {
  struct cache_page *page = NULL;
  return page_cache_get_ra(ip, page_index, 1, PAGE_CACHE_RA_READ, &page) == 0
             ? page
             : NULL;
}

int page_cache_get(struct inode *ip, uint64_t page_index,
                   struct cache_page **out) {
  return page_cache_get_ra(ip, page_index, 1, PAGE_CACHE_RA_READ, out);
}

void page_cache_mark_dirty(struct cache_page *cp) {
  if (!cp)
    return;
  spin_lock(&page_cache_lock);
  cp->flags |= CACHE_PAGE_DIRTY;
  cp->dirty_seq++;
  spin_unlock(&page_cache_lock);
}

int page_cache_writeback(struct cache_page *cp) {
  struct cache_page *pages[] = {cp};
  return page_cache_writeback_pages(pages, 1);
}

int page_cache_writeback_pages(struct cache_page **pages, int nr_pages) {
  if (!pages || nr_pages <= 0 || nr_pages > RA_MAX_PAGES || !pages[0] ||
      !pages[0]->inode)
    return -EINVAL;
  struct inode *ip = pages[0]->inode;
  if (!ip->i_aop || !ip->i_aop->writepages)
    return -EOPNOTSUPP;
  uint32_t snapshots[RA_MAX_PAGES];
  spin_lock(&page_cache_lock);
  for (int i = 0; i < nr_pages; i++) {
    if (!pages[i] || pages[i]->inode != ip || !pages[i]->data) {
      spin_unlock(&page_cache_lock);
      return -EINVAL;
    }
    if (!(pages[i]->flags & CACHE_PAGE_DIRTY)) {
      snapshots[i] = pages[i]->dirty_seq;
      continue;
    }
    snapshots[i] = pages[i]->dirty_seq;
    pages[i]->flags |= CACHE_PAGE_WRITEBACK;
  }
  spin_unlock(&page_cache_lock);

  int rc = ip->i_aop->writepages(ip, pages, (size_t)nr_pages);
  spin_lock(&page_cache_lock);
  for (int i = 0; i < nr_pages; i++) {
    pages[i]->flags &= ~CACHE_PAGE_WRITEBACK;
    if (!rc && pages[i]->dirty_seq == snapshots[i])
      pages[i]->flags &= ~(CACHE_PAGE_DIRTY | CACHE_PAGE_ERROR);
    else if (rc)
      pages[i]->flags |= CACHE_PAGE_ERROR;
  }
  spin_unlock(&page_cache_lock);
  for (int i = 0; i < nr_pages; i++)
    __wake_up(&pages[i]->waiters, 0);
  return rc;
}

void page_cache_release(struct cache_page *cp) {
  if (!cp)
    return;
  spin_lock(&page_cache_lock);
  int refs = atomic_dec_return(&cp->pin_count);
  WARN_ON(refs < 0);
  if (!refs && (cp->flags & CACHE_PAGE_INVALID)) {
    if (cp->data)
      kfree(cp->data);
    cp->data = NULL;
    cp->inode = NULL;
    cp->flags = 0;
    free_list_push(cp);
  }
  spin_unlock(&page_cache_lock);
}

void page_cache_invalidate_inode(struct inode *ip) {
  if (!ip)
    return;
  spin_lock(&page_cache_lock);
  for (int i = 0; i < (1 << PAGE_CACHE_HASH_BITS); i++) {
    struct cache_page **link = &page_cache_hash[i];
    while (*link) {
      struct cache_page *cp = *link;
      if (cp->inode != ip) {
        link = &cp->hash_next;
        continue;
      }
      ra_resolve_locked(cp, true);
      *link = cp->hash_next;
      lru_remove(cp);
      cp->hash_next = NULL;
      cp->flags |= CACHE_PAGE_INVALID;
      if (!atomic_read(&cp->pin_count)) {
        kfree(cp->data);
        cp->data = NULL;
        cp->inode = NULL;
        cp->flags = 0;
        free_list_push(cp);
      }
      __wake_up(&cp->waiters, 0);
    }
  }
  spin_unlock(&page_cache_lock);
}

void page_cache_get_stats(struct page_cache_stats *out) {
  if (!out)
    return;
  spin_lock(&page_cache_lock);
  __memcpy(out, &cache_stats, sizeof(*out));
  spin_unlock(&page_cache_lock);
}

static int collect_dirty(struct inode *only_ip, struct cache_page **out,
                         int max) {
  int count = 0;
  spin_lock(&page_cache_lock);
  for (int i = 0; i < (1 << PAGE_CACHE_HASH_BITS) && count < max; i++) {
    for (struct cache_page *cp = page_cache_hash[i]; cp && count < max;
         cp = cp->hash_next) {
      if ((cp->flags & CACHE_PAGE_DIRTY) && cp->data && cp->inode &&
          (!only_ip || cp->inode == only_ip)) {
        atomic_inc(&cp->pin_count);
        out[count++] = cp;
      }
    }
  }
  spin_unlock(&page_cache_lock);
  return count;
}

static int flush_dirty(struct inode *only_ip) {
  for (;;) {
    struct cache_page *pages[64];
    int count = collect_dirty(only_ip, pages, 64);
    if (!count)
      return 0;
    int first_error = 0;
    for (int i = 0; i < count; i++) {
      int rc = page_cache_writeback(pages[i]);
      if (rc && !first_error)
        first_error = rc;
      page_cache_release(pages[i]);
    }
    if (first_error)
      return first_error;
  }
}

int page_cache_flush_all(void) { return flush_dirty(NULL); }

int page_cache_flush_inode(struct inode *ip) { return flush_dirty(ip); }
