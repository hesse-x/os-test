#include "kernel/inode.h"
#include "kernel/mem/slab.h"
#include "kernel/serial.h"
#include <stddef.h>

static struct inode *inode_hash_table[INODE_HASH_SIZE];
static spinlock_t inode_hash_lock = SPINLOCK_INIT;
static uint32_t next_dev_ino = 1;

static unsigned inode_hash(uint32_t ino) {
    return ino & (INODE_HASH_SIZE - 1);
}

void inode_init(void) {
    for (int i = 0; i < INODE_HASH_SIZE; i++)
        inode_hash_table[i] = NULL;
}

struct inode *inode_lookup(uint32_t ino) {
    unsigned idx = inode_hash(ino);
    spin_lock(&inode_hash_lock);
    struct inode *ip = inode_hash_table[idx];
    while (ip) {
        if (ip->ino == ino) {
            ip->ref_count++;
            spin_unlock(&inode_hash_lock);
            return ip;
        }
        ip = ip->hash_next;
    }
    spin_unlock(&inode_hash_lock);
    return NULL;
}

struct inode *inode_create(uint32_t ino, int type, uint64_t size,
                           uint32_t start_cluster, uint32_t dir_cluster,
                           int dir_entry_idx) {
    struct inode *ip = (struct inode *)kmalloc(sizeof(struct inode));
    if (!ip) return NULL;
    ip->type = type;
    ip->ino = (type == INODE_DEV) ? next_dev_ino++ : ino;
    ip->size = size;
    ip->mode = (type == INODE_DIR) ? 0040755 : (type == INODE_DEV) ? 0020000 : 0100644;
    ip->nlink = 1;
    ip->ref_count = 1;
    ip->i_lock = SPINLOCK_INIT;
    ip->i_priv = NULL;
    ip->start_cluster = start_cluster;
    ip->dir_start_cluster = dir_cluster;
    ip->dir_entry_index = dir_entry_idx;
    ip->hash_next = NULL;
    ip->hash_prev = NULL;

    unsigned idx = inode_hash(ip->ino);
    spin_lock(&inode_hash_lock);
    ip->hash_next = inode_hash_table[idx];
    if (inode_hash_table[idx]) inode_hash_table[idx]->hash_prev = ip;
    inode_hash_table[idx] = ip;
    spin_unlock(&inode_hash_lock);
    return ip;
}

struct inode *inode_get(struct inode *ip) {
    ip->ref_count++;
    return ip;
}

void inode_put(struct inode *ip) {
    if (!ip) return;
    spin_lock(&inode_hash_lock);
    ip->ref_count--;
    if (ip->ref_count <= 0) {
        unsigned idx = inode_hash(ip->ino);
        if (inode_hash_table[idx] == ip)
            inode_hash_table[idx] = ip->hash_next;
        if (ip->hash_prev) ip->hash_prev->hash_next = ip->hash_next;
        if (ip->hash_next) ip->hash_next->hash_prev = ip->hash_prev;
        spin_unlock(&inode_hash_lock);
        kfree(ip);
    } else {
        spin_unlock(&inode_hash_lock);
    }
}
