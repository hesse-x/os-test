#ifndef KERNEL_INODE_H
#define KERNEL_INODE_H

#include <stdint.h>
#include "kernel/spinlock.h"
#include "kernel/atomic.h"

struct shm;  /* forward declaration */

#define INODE_REGULAR  1
#define INODE_DIR      2
#define INODE_DEV      3

struct inode {
    int      type;
    uint32_t ino;
    uint64_t size;
    uint32_t mode;
    int      nlink;
    refcount_t i_count;
    spinlock_t i_lock;
    void    *i_priv;          /* INODE_DEV -> dev_ops*; INODE_REGULAR -> NULL */
    struct shm *shm;          /* INODE_DEV -> shared memory (NULL = no SHM) */

    /* FAT32 metadata (REGULAR/DIR only) */
    uint32_t start_cluster;
    uint32_t dir_start_cluster;
    int      dir_entry_index;

    /* Hash chain */
    struct inode *hash_next;
    struct inode *hash_prev;
};

#define INODE_HASH_BITS  6
#define INODE_HASH_SIZE  (1 << INODE_HASH_BITS)  /* 64 */

void    inode_init(void);
struct inode *inode_lookup(uint32_t ino);
struct inode *inode_create(uint32_t ino, int type, uint64_t size,
                           uint32_t start_cluster, uint32_t dir_cluster,
                           int dir_entry_idx) __must_check;
struct inode *inode_get_or_create(uint32_t ino, int type, uint64_t size,
                                  uint32_t start_cluster, uint32_t dir_cluster,
                                  int dir_entry_idx);
void    inode_put(struct inode *ip);
struct inode *inode_get(struct inode *ip);

#endif
