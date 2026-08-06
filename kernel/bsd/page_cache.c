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
    if (atomic_read(&cp->pin_count) == 0 && !(cp->flags & CACHE_PAGE_DIRTY) &&
        cp->data) {
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

struct cache_page *page_cache_fill(struct inode *ip, uint64_t page_index) {
  /* Check if already cached */
  struct cache_page *cp = page_cache_lookup(ip, page_index);
  if (cp)
    return cp;

  spin_lock(&page_cache_lock);

  /* Double-check under lock */
  unsigned idx = page_cache_hashfn(ip, page_index);
  for (;;) {
    struct cache_page *existing = page_cache_hash[idx];
    while (existing) {
      if (existing->inode == ip && existing->page_index == page_index &&
          existing->data) {
        atomic_inc(&existing->pin_count);
        lru_touch(existing);
        uint32_t flags = existing->flags;
        spin_unlock(&page_cache_lock);
        if (flags & CACHE_PAGE_FILLING)
          page_cache_wait(existing, CACHE_PAGE_FILLING);
        flags = __atomic_load_n(&existing->flags, __ATOMIC_ACQUIRE);
        if (flags & (CACHE_PAGE_ERROR | CACHE_PAGE_INVALID)) {
          page_cache_release(existing);
          return NULL;
        }
        return existing;
      }
      existing = existing->hash_next;
    }
    break;
  }

  /* Get a free cache_page struct */
  struct cache_page *new_cp = free_list_pop();
  if (!new_cp) {
    /* Evict under lock and retry */
    if (page_cache_evict() != 0) {
      spin_unlock(&page_cache_lock);
      return NULL;
    }
    new_cp = free_list_pop();
    if (!new_cp) {
      spin_unlock(&page_cache_lock);
      return NULL;
    }
  }

  /* Allocate data buffer */
  new_cp->data = (uint8_t *)kmalloc(4096);
  if (!new_cp->data) {
    free_list_push(new_cp);
    spin_unlock(&page_cache_lock);
    return NULL;
  }

  new_cp->inode = ip;
  new_cp->page_index = page_index;
  atomic_set(&new_cp->pin_count, 1);
  new_cp->flags = CACHE_PAGE_FILLING;
  new_cp->error = 0;
  new_cp->generation++;

  /* Insert into hash */
  new_cp->hash_next = page_cache_hash[idx];
  page_cache_hash[idx] = new_cp;

  /* Insert into LRU head */
  lru_insert_head(new_cp);

  spin_unlock(&page_cache_lock);

  /* Read from disk. Resolve the page's clusters with the inode's forward-only
   * cursor (O(n) total for sequential reads, not O(n²) from re-walking the
   * chain head every page), then coalesce consecutive clusters into a single
   * AHCI command so a contiguous 4KB page is one blk_read() of 8 sectors,
   * not 8. */
  int io_error = 0;
  if (ip->type == INODE_REGULAR || ip->type == INODE_DIR) {
    uint32_t spc = fat32_sectors_per_cluster();
    uint32_t bpc = fat32_bytes_per_cluster();
    uint32_t clusters_per_page = 4096 / bpc;
    uint32_t cluster_idx = (uint32_t)(page_index * clusters_per_page);

    /* First cluster of the page — if it's past EOF, the whole page is zero. */
    uint32_t first = fat32_walk_chain_cached(ip, cluster_idx);
    if (first < 2 || first >= 0x0FFFFFF8) {
      __memset(new_cp->data, 0, 4096);
    } else {
      uint8_t *dst = new_cp->data;
      uint32_t cl = first;
      uint32_t ci = 0;
      while (ci < clusters_per_page) {
        if (cl < 2 || cl >= 0x0FFFFFF8) {
          /* Past EOF — zero the rest of the page. */
          __memset(dst, 0, (size_t)(clusters_per_page - ci) * bpc);
          break;
        }
        /* Find the longest run of consecutive clusters starting at cl. */
        uint32_t run = 1;
        while (ci + run < clusters_per_page) {
          uint32_t next = fat32_walk_chain_cached(ip, cluster_idx + ci + run);
          if (next < 2 || next >= 0x0FFFFFF8 || next != cl + run)
            break;
          run++;
        }
        uint32_t lba = fat32_data_start_lba() + (cl - 2) * spc;
        if (blk_read(lba, run * spc, dst) != 0) {
          io_error = -EIO;
          break;
        }
        dst += run * bpc;
        ci += run;
        cl = (ci < clusters_per_page)
                 ? fat32_walk_chain_cached(ip, cluster_idx + ci)
                 : 0;
      }
    }
  } else {
    __memset(new_cp->data, 0, 4096);
  }

  spin_lock(&page_cache_lock);
  new_cp->flags &= ~CACHE_PAGE_FILLING;
  if (io_error) {
    new_cp->flags |= CACHE_PAGE_ERROR;
    new_cp->error = io_error;
  } else {
    new_cp->flags |= CACHE_PAGE_UPTODATE;
  }
  spin_unlock(&page_cache_lock);
  __wake_up(&new_cp->waiters, 0);

  if (io_error) {
    page_cache_release(new_cp);
    return NULL;
  }

  return new_cp;
}

int page_cache_get(struct inode *ip, uint64_t page_index,
                   struct cache_page **out) {
  if (!out)
    return -EINVAL;
  *out = page_cache_fill(ip, page_index);
  return *out ? 0 : -EIO;
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

  uint32_t bpc = fat32_bytes_per_cluster();
  uint32_t spc = fat32_sectors_per_cluster();
  uint32_t clusters_per_page = 4096 / bpc;
  uint32_t run_lba = 0;
  uint32_t run_sectors = 0;
  size_t run_bytes = 0;
  bool saw_cluster = false;
  int rc = 0;

  for (int pi = 0; pi < nr_pages && rc == 0; pi++) {
    uint32_t base = (uint32_t)pages[pi]->page_index * clusters_per_page;
    for (uint32_t ci = 0; ci < clusters_per_page; ci++) {
      uint32_t cl = fat32_walk_chain(ip->start_cluster, base + ci);
      if (cl < 2 || cl >= 0x0FFFFFF8) {
        if (!saw_cluster)
          rc = -EIO;
        break;
      }
      saw_cluster = true;
      uint32_t lba = fat32_data_start_lba() + (cl - 2) * spc;
      if (run_sectors != 0 && (lba != run_lba + run_sectors ||
                               run_sectors + spc > AHCI_MAX_SECTORS)) {
        rc = blk_write(run_lba, run_sectors, staging);
        run_sectors = 0;
        run_bytes = 0;
        if (rc)
          break;
      }
      if (run_sectors == 0)
        run_lba = lba;
      __memcpy((uint8_t *)staging + run_bytes,
               pages[pi]->data + (size_t)ci * bpc, bpc);
      run_sectors += spc;
      run_bytes += bpc;
    }
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
void page_cache_flush_all(void) {
  for (;;) {
    struct cache_page *buf[64];
    int n = collect_dirty(NULL, buf, 64);
    if (n == 0)
      break;
    for (int i = 0; i < n; i++) {
      page_cache_writeback(buf[i]);
      page_cache_release(buf[i]);
    }
  }
}

/* Write back only the dirty pages of one inode (fsync(fd)). */
void page_cache_flush_inode(struct inode *ip) {
  for (;;) {
    struct cache_page *buf[64];
    int n = collect_dirty(ip, buf, 64);
    if (n == 0)
      break;
    for (int i = 0; i < n; i++) {
      page_cache_writeback(buf[i]);
      page_cache_release(buf[i]);
    }
  }
}
