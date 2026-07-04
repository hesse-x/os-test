#include "kernel/bsd/page_cache.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/bsd/fat32.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "xos/errno.h"
#include "arch/x64/utils.h"
#include <stddef.h>

/* Hash table for page lookup: (inode, page_index) -> cache_page */
static struct cache_page *page_cache_hash[1 << PAGE_CACHE_HASH_BITS];
static spinlock_t page_cache_lock = SPINLOCK_INIT;

/* LRU list: head = most recent, tail = eviction candidate */
static struct cache_page lru_head;
static struct cache_page lru_tail;
static int lru_inited = 0;

/* Free list for pre-allocated cache_page structs */
static struct cache_page *free_list = NULL;
static int free_count = 0;

static unsigned page_cache_hashfn(struct inode *ip, uint64_t page_index) {
    return ((unsigned)(ip->ino) ^ (unsigned)(page_index)) & ((1 << PAGE_CACHE_HASH_BITS) - 1);
}

static void lru_init(void) {
    lru_head.lru_next = &lru_tail;
    lru_tail.lru_prev = &lru_head;
    lru_head.lru_prev = NULL;
    lru_tail.lru_next = NULL;
    lru_inited = 1;
}

static void lru_remove(struct cache_page *cp) {
    if (cp->lru_prev) cp->lru_prev->lru_next = cp->lru_next;
    if (cp->lru_next) cp->lru_next->lru_prev = cp->lru_prev;
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
    if (!free_list) return NULL;
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
    if (!lru_inited) lru_init();
    for (int i = 0; i < (1 << PAGE_CACHE_HASH_BITS); i++)
        page_cache_hash[i] = NULL;

    /* Pre-allocate cache_page structs (not data buffers) */
    for (int i = 0; i < PAGE_CACHE_SIZE; i++) {
        struct cache_page *cp = (struct cache_page *)kmalloc(sizeof(struct cache_page));
        if (!cp) break;
        __memset(cp, 0, sizeof(*cp));
        cp->inode = NULL;
        cp->data = NULL;
        atomic_set(&cp->pin_count, 0);
        cp->dirty = false;
        free_list_push(cp);
    }
    printk(LOG_INFO, "page_cache_init: %d cache_page structs pre-allocated\n", free_count);
}

struct cache_page *page_cache_lookup(struct inode *ip, uint64_t page_index) {
    unsigned idx = page_cache_hashfn(ip, page_index);
    spin_lock(&page_cache_lock);
    for (;;) {
        struct cache_page *cp = page_cache_hash[idx];
        while (cp) {
            if (cp->inode == ip && cp->page_index == page_index && cp->data) {
                /* Wait while page is being filled from disk.
                 * We hold page_cache_lock; the filler clears filling
                 * with an atomic store (no lock needed), so we will
                 * see the update via atomic loads. */
                while (__atomic_load_n(&cp->filling, __ATOMIC_ACQUIRE))
                    __asm__ volatile("pause");
                atomic_inc(&cp->pin_count);
                lru_touch(cp);
                spin_unlock(&page_cache_lock);
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
        if (atomic_read(&cp->pin_count) == 0 && !cp->dirty && cp->data) {
            /* Remove from hash */
            unsigned idx = page_cache_hashfn(cp->inode, cp->page_index);
            struct cache_page **pp = &page_cache_hash[idx];
            while (*pp) {
                if (*pp == cp) { *pp = cp->hash_next; break; }
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
    if (cp) return cp;

    spin_lock(&page_cache_lock);

    /* Double-check under lock */
    unsigned idx = page_cache_hashfn(ip, page_index);
    for (;;) {
        struct cache_page *existing = page_cache_hash[idx];
        while (existing) {
            if (existing->inode == ip && existing->page_index == page_index && existing->data) {
                /* Wait while another CPU is filling this page */
                while (__atomic_load_n(&existing->filling, __ATOMIC_ACQUIRE))
                    __asm__ volatile("pause");
                atomic_inc(&existing->pin_count);
                lru_touch(existing);
                spin_unlock(&page_cache_lock);
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
    new_cp->dirty = false;
    new_cp->filling = true;   /* visible in hash but data not ready yet */

    /* Insert into hash */
    new_cp->hash_next = page_cache_hash[idx];
    page_cache_hash[idx] = new_cp;

    /* Insert into LRU head */
    lru_insert_head(new_cp);

    spin_unlock(&page_cache_lock);

    /* Read from disk — need to walk FAT chain to find the right cluster */
    if (ip->type == INODE_REGULAR || ip->type == INODE_DIR) {
        /* page_index is in 4096-byte units; each cluster may be smaller.
           Convert to cluster index: cluster_idx = page_index * (4096 / bytes_per_cluster) */
        uint32_t clusters_per_page = 4096 / fat32_bytes_per_cluster();
        uint32_t cluster_idx = page_index * clusters_per_page;
        uint32_t target_cluster = fat32_walk_chain(ip->start_cluster, cluster_idx);
        if (target_cluster < 2 || target_cluster >= 0x0FFFFFF8) {
            /* Beyond file — zero fill (for write extend) */
            __memset(new_cp->data, 0, 4096);
        } else {
            /* Read all clusters that fit in this 4096-byte page */
            uint8_t *dst = new_cp->data;
            for (uint32_t ci = 0; ci < clusters_per_page; ci++) {
                uint32_t cl = fat32_walk_chain(ip->start_cluster, cluster_idx + ci);
                if (cl < 2 || cl >= 0x0FFFFFF8) {
                    /* No more clusters — zero remaining */
                    __memset(dst, 0, fat32_bytes_per_cluster());
                } else {
                    uint32_t lba = fat32_data_start_lba() +
                                   (cl - 2) * fat32_sectors_per_cluster();
                    if (blk_read(lba, fat32_sectors_per_cluster(), dst) != 0) {
                        __memset(dst, 0, fat32_bytes_per_cluster());
                    }
                }
                dst += fat32_bytes_per_cluster();
            }
        }
    } else {
        __memset(new_cp->data, 0, 4096);
    }

    /* Data is now ready — clear filling flag so others can use this page.
     * No lock needed: filling only transitions true→false, and waiters
     * spin-read it with atomic loads for visibility. */
    __atomic_store_n(&new_cp->filling, false, __ATOMIC_RELEASE);

    return new_cp;
}

void page_cache_mark_dirty(struct cache_page *cp) {
    spin_lock(&page_cache_lock);
    cp->dirty = true;
    spin_unlock(&page_cache_lock);
}

int page_cache_writeback(struct cache_page *cp) {
    if (!cp->dirty || !cp->data || !cp->inode) return 0;

    struct inode *ip = cp->inode;
    if (ip->type != INODE_REGULAR && ip->type != INODE_DIR) return 0;

    uint32_t clusters_per_page = 4096 / fat32_bytes_per_cluster();
    uint32_t cluster_idx = cp->page_index * clusters_per_page;
    uint8_t *src = cp->data;

    for (uint32_t ci = 0; ci < clusters_per_page; ci++) {
        uint32_t cl = fat32_walk_chain(ip->start_cluster, cluster_idx + ci);
        if (cl < 2 || cl >= 0x0FFFFFF8) break;

        uint32_t lba = fat32_data_start_lba() +
                       (cl - 2) * fat32_sectors_per_cluster();
        blk_write(lba, fat32_sectors_per_cluster(), src);
        src += fat32_bytes_per_cluster();
    }

    int rc = 0;
    if (rc == 0) {
        spin_lock(&page_cache_lock);
        cp->dirty = false;
        spin_unlock(&page_cache_lock);
    }
    return rc;
}

void page_cache_release(struct cache_page *cp) {
    if (!cp) return;
    WARN_ON(atomic_read(&cp->pin_count) <= 0);
    spin_lock(&page_cache_lock);
    int old = atomic_dec_return(&cp->pin_count);
    WARN_ON(old < 0);
    spin_unlock(&page_cache_lock);
}

void page_cache_invalidate_inode(struct inode *ip) {
    spin_lock(&page_cache_lock);
    for (int i = 0; i < (1 << PAGE_CACHE_HASH_BITS); i++) {
        struct cache_page **pp = &page_cache_hash[i];
        while (*pp) {
            struct cache_page *cp = *pp;
            if (cp->inode == ip) {
                *pp = cp->hash_next;
                lru_remove(cp);
                if (cp->data) kfree(cp->data);
                cp->data = NULL;
                cp->inode = NULL;
                free_list_push(cp);
            } else {
                pp = &cp->hash_next;
            }
        }
    }
    spin_unlock(&page_cache_lock);
}
