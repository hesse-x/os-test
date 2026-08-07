/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/fat32.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/driver/ahci.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"

#include <xos/errno.h>
#include <xos/page.h>

/* Hash table for page lookup: (inode, page_index) -> cache_page */
static struct cache_page *page_cache_hash[1 << PAGE_CACHE_HASH_BITS];
static spinlock page_cache_lock = SPINLOCK_INIT;

/* LRU list: head = most recent, tail = eviction candidate */
static struct cache_page lru_head;
static struct cache_page lru_tail;
static int lru_inited = 0;

/* Free list for pre-allocated cache_page structs */
static struct cache_page *free_list = NULL;
static int free_count = 0;

#define RA_MAX_PAGES 16
static int ra_staging_in_use;
static struct page_cache_stats cache_stats;

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

static unsigned page_cache_hashfn(struct inode *ip, uint64_t page_index) {
  return ((unsigned)(ip->ino) ^ (unsigned)(page_index)) &
         ((1 << PAGE_CACHE_HASH_BITS) - 1);
}

static void lru_init(void) {
  lru_head.lru_next = &lru_tail;
  lru_tail.lru_prev = &lru_head;
  lru_head.lru_prev = NULL;
  lru_tail.lru_next = NULL;
  lru_inited = 1;
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

/* Move to head of LRU (most recently used) */
static void lru_touch(struct cache_page *cp) {
  lru_remove(cp);
  lru_insert_head(cp);
}

static struct cache_page *free_list_pop(void) {
  if (!free_list)
    return NULL;
  struct cache_page *cp = free_list;
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

void page_cache_init(void) {
  if (!lru_inited)
    lru_init();
  for (int i = 0; i < (1 << PAGE_CACHE_HASH_BITS); i++)
    page_cache_hash[i] = NULL;

  /* Pre-allocate cache_page structs (not data buffers) */
  for (int i = 0; i < PAGE_CACHE_SIZE; i++) {
    struct cache_page *cp =
        (struct cache_page *)kmalloc(sizeof(struct cache_page));
    if (!cp)
      break;
    __memset(cp, 0, sizeof(*cp));
    cp->inode = NULL;
    cp->data = NULL;
    atomic_set(&cp->pin_count, 0);
    cp->flags = 0;
    init_wait_queue_head(&cp->waiters);
    free_list_push(cp);
  }
  printk(LOG_INFO, "page_cache_init: %d cache_page structs pre-allocated\n",
         free_count);
}

struct cache_page *page_cache_lookup(struct inode *ip, uint64_t page_index) {
  unsigned idx = page_cache_hashfn(ip, page_index);
  spin_lock(&page_cache_lock);
  for (;;) {
    struct cache_page *cp = page_cache_hash[idx];
    while (cp) {
      if (cp->inode == ip && cp->page_index == page_index && cp->data) {
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
        return cp;
      }
      cp = cp->hash_next;
    }
    break; /* not found in hash */
  }
  spin_unlock(&page_cache_lock);
  return NULL;
}

static int page_cache_evict(void) {
  /* Walk from LRU tail (least recently used), find first evictable page */
  struct cache_page *cp = lru_tail.lru_prev;
  while (cp != &lru_head) {
    if (atomic_read(&cp->pin_count) == 0 &&
        !(cp->flags &
          (CACHE_PAGE_FILLING | CACHE_PAGE_WRITEBACK | CACHE_PAGE_DIRTY)) &&
        cp->data) {
      if (cp->flags & CACHE_PAGE_READAHEAD)
        __atomic_fetch_add(&cache_stats.readahead_waste, 1, __ATOMIC_RELAXED);
      /* Remove from hash */
      unsigned idx = page_cache_hashfn(cp->inode, cp->page_index);
      struct cache_page **pp = &page_cache_hash[idx];
      while (*pp) {
        if (*pp == cp) {
          *pp = cp->hash_next;
          break;
        }
        pp = &(*pp)->hash_next;
      }
      /* Remove from LRU */
      lru_remove(cp);
      /* Free data buffer */
      kfree(cp->data);
      cp->data = NULL;
      cp->inode = NULL;
      cp->page_index = 0;
      /* Return to free list */
      free_list_push(cp);
      return 0;
    }
    cp = cp->lru_prev;
  }
  return -1; /* nothing evictable */
}

static struct cache_page *
page_cache_alloc_locked(struct inode *ip, uint64_t page_index, bool readahead) {
  struct cache_page *cp = free_list_pop();
  if (!cp) {
    if (page_cache_evict() != 0)
      return NULL;
    cp = free_list_pop();
  }
  if (!cp)
    return NULL;
  cp->data = (uint8_t *)kmalloc(PAGE_SIZE);
  if (!cp->data) {
    free_list_push(cp);
    return NULL;
  }
  cp->inode = ip;
  cp->page_index = page_index;
  atomic_set(&cp->pin_count, 1); /* fill owner */
  cp->flags = CACHE_PAGE_FILLING | (readahead ? CACHE_PAGE_READAHEAD : 0);
  cp->error = 0;
  cp->generation++;
  unsigned idx = page_cache_hashfn(ip, page_index);
  cp->hash_next = page_cache_hash[idx];
  page_cache_hash[idx] = cp;
  lru_insert_head(cp);
  return cp;
}

static void page_cache_unhash_locked(struct cache_page *cp) {
  unsigned idx = page_cache_hashfn(cp->inode, cp->page_index);
  struct cache_page **pp = &page_cache_hash[idx];
  while (*pp && *pp != cp)
    pp = &(*pp)->hash_next;
  if (*pp == cp)
    *pp = cp->hash_next;
  cp->hash_next = NULL;
  lru_remove(cp);
  cp->flags |= CACHE_PAGE_INVALID;
}

static bool page_sector_lba(struct inode *ip, uint64_t file_sector,
                            uint32_t *lba) {
  uint32_t spc = fat32_sectors_per_cluster();
  if (!spc)
    return false;
  uint64_t cluster_index = file_sector / spc;
  if (cluster_index > UINT32_MAX)
    return false;
  uint32_t cluster = fat32_walk_chain_cached(ip, cluster_index);
  if (cluster < 2 || cluster >= 0x0FFFFFF8)
    return false;
  uint64_t disk_lba = (uint64_t)fat32_data_start_lba() +
                      (uint64_t)(cluster - 2) * spc + file_sector % spc;
  if (disk_lba > UINT32_MAX)
    return false;
  *lba = (uint32_t)disk_lba;
  return true;
}

static int read_mapped_sectors(struct inode *ip, uint64_t first_file_sector,
                               uint32_t sectors, uint8_t *dst) {
  uint32_t done = 0;
  while (done < sectors) {
    uint32_t lba;
    if (!page_sector_lba(ip, first_file_sector + done, &lba))
      return -EIO;
    uint32_t run = 1;
    while (done + run < sectors && run < AHCI_MAX_SECTORS) {
      uint32_t next;
      if (!page_sector_lba(ip, first_file_sector + done + run, &next) ||
          next != lba + run)
        break;
      run++;
    }
    if (blk_read(lba, run, dst + (size_t)done * 512) != 0)
      return -EIO;
    done += run;
  }
  return 0;
}

int page_cache_get_ra(struct inode *ip, uint64_t page_index,
                      uint32_t window_pages, struct cache_page **out) {
  if (!ip || !out || window_pages == 0 || window_pages > RA_MAX_PAGES)
    return -EINVAL;
  *out = page_cache_lookup(ip, page_index);
  if (*out) {
    uint32_t old = __atomic_fetch_and(&(*out)->flags, ~CACHE_PAGE_READAHEAD,
                                      __ATOMIC_RELAXED);
    if (old & CACHE_PAGE_READAHEAD)
      __atomic_fetch_add(&cache_stats.readahead_hits, 1, __ATOMIC_RELAXED);
    return 0;
  }

  if (ip->type != INODE_REGULAR)
    window_pages = 1;
  if (ip->type == INODE_REGULAR) {
    uint64_t file_pages = (ip->size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (page_index >= file_pages)
      window_pages = 1;
    else if (window_pages > file_pages - page_index)
      window_pages = (uint32_t)(file_pages - page_index);
  }

  struct cache_page *pages[RA_MAX_PAGES] = {0};
  uint32_t nr = 0;
  spin_lock(&page_cache_lock);
  for (; nr < window_pages; nr++) {
    uint64_t index = page_index + nr;
    unsigned bucket = page_cache_hashfn(ip, index);
    struct cache_page *existing = page_cache_hash[bucket];
    while (existing && (existing->inode != ip ||
                        existing->page_index != index || !existing->data))
      existing = existing->hash_next;
    if (existing) {
      if (nr == 0) {
        atomic_inc(&existing->pin_count);
        lru_touch(existing);
        pages[0] = existing;
      }
      break; /* never overwrite or read across another fill owner */
    }
    pages[nr] = page_cache_alloc_locked(ip, index, nr != 0);
    if (!pages[nr])
      break;
  }
  spin_unlock(&page_cache_lock);

  if (nr == 0 && pages[0]) {
    uint32_t flags = __atomic_load_n(&pages[0]->flags, __ATOMIC_ACQUIRE);
    if (flags & CACHE_PAGE_FILLING)
      page_cache_wait(pages[0], CACHE_PAGE_FILLING);
    if (__atomic_load_n(&pages[0]->flags, __ATOMIC_ACQUIRE) &
        (CACHE_PAGE_ERROR | CACHE_PAGE_INVALID)) {
      page_cache_release(pages[0]);
      return -EIO;
    }
    *out = pages[0];
    return 0;
  }
  if (nr == 0)
    return -ENOMEM;

  uint64_t byte_start = page_index * (uint64_t)PAGE_SIZE;
  uint32_t sectors = nr * (PAGE_SIZE / 512);
  if (ip->type == INODE_REGULAR) {
    if (byte_start >= ip->size) {
      sectors = 0;
    } else {
      uint64_t available = ip->size - byte_start;
      uint64_t wanted = (uint64_t)sectors * 512;
      if (available < wanted)
        sectors = (uint32_t)((available + 511) / 512);
    }
  }

  void *staging = NULL;
  bool staging_slot = false;
  if (nr > 1) {
    int old = __atomic_fetch_add(&ra_staging_in_use, 1, __ATOMIC_ACQ_REL);
    if (old < 4) {
      staging_slot = true;
      staging = bfc_alloc_page_data(RA_MAX_PAGES);
    } else {
      __atomic_fetch_sub(&ra_staging_in_use, 1, __ATOMIC_RELEASE);
    }
    if (!staging) {
      if (staging_slot)
        __atomic_fetch_sub(&ra_staging_in_use, 1, __ATOMIC_RELEASE);
      /* Resource pressure is not allowed to turn speculative I/O into a
       * demand failure. Keep the demand reservation and discard the tail. */
      spin_lock(&page_cache_lock);
      for (uint32_t i = 1; i < nr; i++)
        page_cache_unhash_locked(pages[i]);
      spin_unlock(&page_cache_lock);
      for (uint32_t i = 1; i < nr; i++) {
        __wake_up(&pages[i]->waiters, 0);
        page_cache_release(pages[i]);
      }
      nr = 1;
      sectors = sectors > 8 ? 8 : sectors;
      __atomic_fetch_add(&cache_stats.readahead_fallbacks, 1, __ATOMIC_RELAXED);
    }
  }

  uint8_t *io_buf = staging ? (uint8_t *)staging : pages[0]->data;
  __memset(io_buf, 0, (size_t)nr * PAGE_SIZE);

  /* Speculation is admitted only while sectors remain physically adjacent.
   * A fragmented demand page is still read completely, but suppresses its
   * speculative tail. */
  uint32_t admitted_pages = nr;
  bool fragmented_demand = false;
  uint32_t prev_lba = 0;
  bool mapping_failed = false;
  for (uint32_t s = 0; s < sectors; s++) {
    uint32_t lba;
    if (!page_sector_lba(ip, byte_start / 512 + s, &lba)) {
      mapping_failed = true;
      sectors = s;
      admitted_pages = s < 8 ? 1 : s / 8;
      break;
    }
    if (s && lba != prev_lba + 1) {
      if (s < 8)
        fragmented_demand = true;
      else {
        admitted_pages = fragmented_demand ? 1 : s / 8;
        sectors = admitted_pages * 8;
        break;
      }
    }
    prev_lba = lba;
  }
  if (fragmented_demand && admitted_pages > 1) {
    admitted_pages = 1;
    sectors = sectors > 8 ? 8 : sectors;
  }

  if (admitted_pages < nr) {
    __atomic_fetch_add(&cache_stats.readahead_fragment_truncations, 1,
                       __ATOMIC_RELAXED);
    spin_lock(&page_cache_lock);
    for (uint32_t i = admitted_pages; i < nr; i++)
      page_cache_unhash_locked(pages[i]);
    spin_unlock(&page_cache_lock);
    for (uint32_t i = admitted_pages; i < nr; i++) {
      __wake_up(&pages[i]->waiters, 0);
      page_cache_release(pages[i]);
    }
    nr = admitted_pages;
  }

  int io_error = 0;
  if (ip->type == INODE_REGULAR || ip->type == INODE_DIR) {
    if (mapping_failed && sectors < 8 && byte_start < ip->size)
      io_error = -EIO;
    else if (sectors)
      io_error = read_mapped_sectors(ip, byte_start / 512, sectors, io_buf);
  }
  if (staging && !io_error) {
    for (uint32_t i = 0; i < nr; i++)
      __memcpy(pages[i]->data, io_buf + (size_t)i * PAGE_SIZE, PAGE_SIZE);
  }
  if (staging) {
    bfc_free_page_data(staging, RA_MAX_PAGES);
    __atomic_fetch_sub(&ra_staging_in_use, 1, __ATOMIC_RELEASE);
  }

  spin_lock(&page_cache_lock);
  for (uint32_t i = 0; i < nr; i++) {
    pages[i]->flags &= ~CACHE_PAGE_FILLING;
    if (io_error) {
      pages[i]->flags |= CACHE_PAGE_ERROR;
      pages[i]->error = io_error;
      page_cache_unhash_locked(pages[i]);
    } else {
      pages[i]->flags |= CACHE_PAGE_UPTODATE;
    }
  }
  spin_unlock(&page_cache_lock);
  for (uint32_t i = 0; i < nr; i++)
    __wake_up(&pages[i]->waiters, 0);

  if (io_error) {
    for (uint32_t i = 0; i < nr; i++)
      page_cache_release(pages[i]);
    return io_error;
  }
  /* Speculative pages no longer need the fill owner's pin. */
  for (uint32_t i = 1; i < nr; i++)
    page_cache_release(pages[i]);
  if (nr > 1) {
    __atomic_fetch_add(&cache_stats.readahead_batches, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&cache_stats.readahead_pages, nr - 1, __ATOMIC_RELAXED);
  }
  *out = pages[0];
  return 0;
}

struct cache_page *page_cache_fill(struct inode *ip, uint64_t page_index) {
  struct cache_page *cp = NULL;
  return page_cache_get_ra(ip, page_index, 1, &cp) == 0 ? cp : NULL;
}

int page_cache_get(struct inode *ip, uint64_t page_index,
                   struct cache_page **out) {
  return page_cache_get_ra(ip, page_index, 1, out);
}

void page_cache_mark_dirty(struct cache_page *cp) {
  spin_lock(&page_cache_lock);
  cp->flags |= CACHE_PAGE_DIRTY;
  cp->dirty_seq++;
  spin_unlock(&page_cache_lock);
}

int page_cache_writeback(struct cache_page *cp) {
  struct cache_page *pages[1] = {cp};
  return page_cache_writeback_pages(pages, 1);
}

int page_cache_writeback_pages(struct cache_page **pages, int nr_pages) {
  if (!pages || nr_pages <= 0 || nr_pages > 16)
    return -EINVAL;
  struct cache_page *first = pages[0];
  if (!(first->flags & CACHE_PAGE_DIRTY) || !first->data || !first->inode)
    return 0;

  struct inode *ip = first->inode;
  if (ip->type != INODE_REGULAR && ip->type != INODE_DIR)
    return 0;

  uint32_t snapshots[16];
  spin_lock(&page_cache_lock);
  for (int i = 0; i < nr_pages; i++) {
    if (!pages[i] || pages[i]->inode != ip || !pages[i]->data ||
        (i > 0 && pages[i]->page_index != pages[i - 1]->page_index + 1)) {
      spin_unlock(&page_cache_lock);
      return -EINVAL;
    }
    snapshots[i] = pages[i]->dirty_seq;
    pages[i]->flags |= CACHE_PAGE_WRITEBACK;
  }
  spin_unlock(&page_cache_lock);

  void *staging = bfc_alloc_page_data(16);
  if (!staging) {
    spin_lock(&page_cache_lock);
    for (int i = 0; i < nr_pages; i++)
      pages[i]->flags &= ~CACHE_PAGE_WRITEBACK;
    spin_unlock(&page_cache_lock);
    return -ENOMEM;
  }

  uint32_t run_lba = 0;
  uint32_t run_sectors = 0;
  int rc = 0;
  uint32_t total_sectors = (uint32_t)nr_pages * (PAGE_SIZE / 512);
  uint64_t first_file_sector = pages[0]->page_index * (PAGE_SIZE / 512);
  for (uint32_t s = 0; s < total_sectors && rc == 0; s++) {
    uint32_t lba;
    if (!page_sector_lba(ip, first_file_sector + s, &lba)) {
      /* FAT32 only allocates clusters covering the written byte range.  The
       * final cache page therefore commonly ends before its 4 KiB boundary.
       * Missing sectors after at least one mapped sector are the allocation
       * boundary, not an I/O error. */
      if (s == 0)
        rc = -EIO;
      break;
    }
    if (run_sectors &&
        (lba != run_lba + run_sectors || run_sectors == AHCI_MAX_SECTORS)) {
      rc = blk_write(run_lba, run_sectors, staging);
      run_sectors = 0;
      if (rc)
        break;
    }
    if (!run_sectors)
      run_lba = lba;
    uint32_t pi = s / (PAGE_SIZE / 512);
    uint32_t page_sector = s % (PAGE_SIZE / 512);
    __memcpy((uint8_t *)staging + (size_t)run_sectors * 512,
             pages[pi]->data + (size_t)page_sector * 512, 512);
    run_sectors++;
  }
  if (rc == 0 && run_sectors != 0)
    rc = blk_write(run_lba, run_sectors, staging);

  bfc_free_page_data(staging, 16);

  spin_lock(&page_cache_lock);
  for (int i = 0; i < nr_pages; i++) {
    pages[i]->flags &= ~CACHE_PAGE_WRITEBACK;
    if (rc == 0 && pages[i]->dirty_seq == snapshots[i])
      pages[i]->flags &= ~(CACHE_PAGE_DIRTY | CACHE_PAGE_ERROR);
    else if (rc != 0)
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
  WARN_ON(atomic_read(&cp->pin_count) <= 0);
  spin_lock(&page_cache_lock);
  int old = atomic_dec_return(&cp->pin_count);
  WARN_ON(old < 0);
  if (old == 0 && (cp->flags & CACHE_PAGE_INVALID)) {
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
  /* Drop any stale FAT-chain walk cursor too: callers invoke this precisely
   * when the cluster chain changes (write-extend, truncate, unlink, rename),
   * so a retained cursor could point into freed/realigned clusters. */
  __atomic_store_n(&ip->walk_cursor, 0, __ATOMIC_RELEASE);
  spin_lock(&page_cache_lock);
  for (int i = 0; i < (1 << PAGE_CACHE_HASH_BITS); i++) {
    struct cache_page **pp = &page_cache_hash[i];
    while (*pp) {
      struct cache_page *cp = *pp;
      if (cp->inode == ip) {
        if (cp->flags & CACHE_PAGE_READAHEAD)
          __atomic_fetch_add(&cache_stats.readahead_waste, 1, __ATOMIC_RELAXED);
        *pp = cp->hash_next;
        lru_remove(cp);
        cp->hash_next = NULL;
        cp->flags |= CACHE_PAGE_INVALID;
        if (atomic_read(&cp->pin_count) == 0) {
          if (cp->data)
            kfree(cp->data);
          cp->data = NULL;
          cp->inode = NULL;
          cp->flags = 0;
          free_list_push(cp);
        }
        __wake_up(&cp->waiters, 0);
      } else {
        pp = &cp->hash_next;
      }
    }
  }
  spin_unlock(&page_cache_lock);
}

void page_cache_get_stats(struct page_cache_stats *out) {
  if (!out)
    return;
  out->readahead_batches =
      __atomic_load_n(&cache_stats.readahead_batches, __ATOMIC_RELAXED);
  out->readahead_pages =
      __atomic_load_n(&cache_stats.readahead_pages, __ATOMIC_RELAXED);
  out->readahead_hits =
      __atomic_load_n(&cache_stats.readahead_hits, __ATOMIC_RELAXED);
  out->readahead_waste =
      __atomic_load_n(&cache_stats.readahead_waste, __ATOMIC_RELAXED);
  out->readahead_fragment_truncations = __atomic_load_n(
      &cache_stats.readahead_fragment_truncations, __ATOMIC_RELAXED);
  out->readahead_fallbacks =
      __atomic_load_n(&cache_stats.readahead_fallbacks, __ATOMIC_RELAXED);
}

/* Collect dirty pages (optionally restricted to one inode) into out[], pinning
 * each so it can't be evicted before writeback. Returns the count collected,
 * capped at max. Caller must page_cache_release() each collected page. */
static int collect_dirty(struct inode *only_ip, struct cache_page **out,
                         int max) {
  int n = 0;
  spin_lock(&page_cache_lock);
  for (int i = 0; i < (1 << PAGE_CACHE_HASH_BITS) && n < max; i++) {
    for (struct cache_page *cp = page_cache_hash[i]; cp && n < max;
         cp = cp->hash_next) {
      if ((cp->flags & CACHE_PAGE_DIRTY) && cp->data && cp->inode &&
          (!only_ip || cp->inode == only_ip)) {
        atomic_inc(&cp->pin_count);
        out[n++] = cp;
      }
    }
  }
  spin_unlock(&page_cache_lock);
  return n;
}

/* Write back every dirty page in the cache (sync()). Walks all 64 hash buckets
 * — there is no global dirty list yet (vfs.md todo). */
int page_cache_flush_all(void) {
  for (;;) {
    struct cache_page *buf[64];
    int n = collect_dirty(NULL, buf, 64);
    if (n == 0)
      return 0;
    int first_error = 0;
    for (int i = 0; i < n; i++) {
      int rc = page_cache_writeback(buf[i]);
      if (rc && !first_error)
        first_error = rc;
      page_cache_release(buf[i]);
    }
    if (first_error)
      return first_error;
  }
}

/* Write back only the dirty pages of one inode (fsync(fd)). */
int page_cache_flush_inode(struct inode *ip) {
  for (;;) {
    struct cache_page *buf[64];
    int n = collect_dirty(ip, buf, 64);
    if (n == 0)
      return 0;
    int first_error = 0;
    for (int i = 0; i < n; i++) {
      int rc = page_cache_writeback(buf[i]);
      if (rc && !first_error)
        first_error = rc;
      page_cache_release(buf[i]);
    }
    if (first_error)
      return first_error;
  }
}
