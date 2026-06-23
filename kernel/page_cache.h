#ifndef KERNEL_PAGE_CACHE_H
#define KERNEL_PAGE_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include "kernel/inode.h"

struct cache_page {
    struct inode   *inode;
    uint64_t        page_index;
    uint8_t        *data;          /* kmalloc(4096) on fill, kfree on evict */
    int             pin_count;
    bool            dirty;
    struct cache_page *hash_next;
    struct cache_page *lru_prev;
    struct cache_page *lru_next;
};

#define PAGE_CACHE_HASH_BITS  6
#define PAGE_CACHE_SIZE       1024

void    page_cache_init(void);
struct cache_page *page_cache_lookup(struct inode *ip, uint64_t page_index);
struct cache_page *page_cache_fill(struct inode *ip, uint64_t page_index);
int     page_cache_evict(void);
void    page_cache_mark_dirty(struct cache_page *cp);
int     page_cache_writeback(struct cache_page *cp);
void    page_cache_invalidate_inode(struct inode *ip);
void    page_cache_release(struct cache_page *cp);

#endif
