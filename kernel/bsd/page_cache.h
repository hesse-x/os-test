/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_PAGE_CACHE_H
#define KERNEL_PAGE_CACHE_H

#include <stdint.h>

#include "kernel/xcore/atomic.h"
#include "kernel/xcore/wait_queue.h"

struct inode;

#define CACHE_PAGE_FILLING (1U << 0)
#define CACHE_PAGE_UPTODATE (1U << 1)
#define CACHE_PAGE_DIRTY (1U << 2)
#define CACHE_PAGE_WRITEBACK (1U << 3)
#define CACHE_PAGE_ERROR (1U << 4)
#define CACHE_PAGE_INVALID (1U << 5)
#define CACHE_PAGE_READAHEAD (1U << 6)

struct cache_page {
  struct inode *inode;
  uint64_t page_index;
  uint8_t *data; /* kmalloc(4096) on fill, kfree on evict */
  atomic_t pin_count;
  uint32_t flags;
  int error;
  uint32_t generation;
  uint32_t dirty_seq;
  wait_queue_head waiters;
  struct cache_page *hash_next;
  struct cache_page *lru_prev;
  struct cache_page *lru_next;
};

struct page_cache_stats {
  uint64_t readahead_batches;
  uint64_t readahead_pages;
  uint64_t readahead_hits;
  uint64_t readahead_waste;
  uint64_t readahead_fragment_truncations;
  uint64_t readahead_fallbacks;
};

#define PAGE_CACHE_HASH_BITS 6
#define PAGE_CACHE_SIZE 1024

void page_cache_init(void);
struct cache_page *page_cache_lookup(struct inode *ip, uint64_t page_index);
struct cache_page *page_cache_fill(struct inode *ip, uint64_t page_index);
int page_cache_get(struct inode *ip, uint64_t page_index,
                   struct cache_page **out);
int page_cache_get_ra(struct inode *ip, uint64_t page_index,
                      uint32_t window_pages, struct cache_page **out);
void page_cache_mark_dirty(struct cache_page *cp);
int page_cache_writeback(struct cache_page *cp);
int page_cache_writeback_pages(struct cache_page **pages, int nr_pages);
void page_cache_invalidate_inode(struct inode *ip);
void page_cache_release(struct cache_page *cp);
int page_cache_flush_all(void); // sync(): write back all dirty pages
int page_cache_flush_inode(struct inode *ip); // fsync(fd): one inode
void page_cache_get_stats(struct page_cache_stats *out);

#endif
