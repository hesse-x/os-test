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

#define PAGE_CACHE_RA_MAX_PAGES 16

#ifndef XOS_RA_MAX_PAGES
#define XOS_RA_MAX_PAGES PAGE_CACHE_RA_MAX_PAGES
#endif

enum page_cache_ra_source {
  PAGE_CACHE_RA_MMAP = 0,
  PAGE_CACHE_RA_READ = 1,
  PAGE_CACHE_RA_SOURCE_COUNT,
};

enum page_cache_ra_bucket {
  PAGE_CACHE_RA_BUCKET_1 = 0,
  PAGE_CACHE_RA_BUCKET_4,
  PAGE_CACHE_RA_BUCKET_8,
  PAGE_CACHE_RA_BUCKET_16,
  PAGE_CACHE_RA_BUCKET_OTHER,
  PAGE_CACHE_RA_BUCKET_COUNT,
};

struct cache_page {
  struct inode *inode;
  uint64_t page_index;
  uint8_t *data; // kmalloc(4096) on fill, kfree on evict
  atomic_t pin_count;
  uint32_t flags;
  int error;
  uint32_t generation;
  uint32_t dirty_seq;
  uint8_t ra_source;
  uint8_t ra_bucket;
  uint8_t ra_lifecycle;
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
  uint64_t calls[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t requested_pages[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t admitted_demand[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t admitted_speculative[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t hits[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t eviction_waste[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t invalidation_waste[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t outstanding[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t outstanding_peak[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t requested_window[PAGE_CACHE_RA_SOURCE_COUNT]
                           [PAGE_CACHE_RA_BUCKET_COUNT];
  uint64_t effective_window[PAGE_CACHE_RA_SOURCE_COUNT]
                           [PAGE_CACHE_RA_BUCKET_COUNT];
  uint64_t admitted_window[PAGE_CACHE_RA_SOURCE_COUNT]
                          [PAGE_CACHE_RA_BUCKET_COUNT];
  uint64_t fragment_truncations[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t reservation_conflicts[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t staging_fallbacks[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t batch_io_commands[PAGE_CACHE_RA_SOURCE_COUNT];
  uint64_t batch_io_sectors[PAGE_CACHE_RA_SOURCE_COUNT];
};

#define PAGE_CACHE_HASH_BITS 6
#define PAGE_CACHE_SIZE 1024

void page_cache_init(void);
struct cache_page *page_cache_lookup(struct inode *ip, uint64_t page_index);
struct cache_page *page_cache_fill(struct inode *ip, uint64_t page_index);
int page_cache_get(struct inode *ip, uint64_t page_index,
                   struct cache_page **out);
int page_cache_get_ra(struct inode *ip, uint64_t page_index,
                      uint32_t window_pages, enum page_cache_ra_source source,
                      struct cache_page **out);
uint32_t page_cache_ra_cap(uint32_t requested_pages);
void page_cache_mark_dirty(struct cache_page *cp);
int page_cache_writeback(struct cache_page *cp);
int page_cache_writeback_pages(struct cache_page **pages, int nr_pages);
void page_cache_invalidate_inode(struct inode *ip);
void page_cache_release(struct cache_page *cp);
int page_cache_flush_all(void); // sync(): write back all dirty pages
int page_cache_flush_inode(struct inode *ip); // fsync(fd): one inode
void page_cache_get_stats(struct page_cache_stats *out);

#endif
