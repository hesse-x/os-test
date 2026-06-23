#include "kernel/devtmpfs.h"
#include "kernel/inode.h"
#include "kernel/proc.h"
#include "kernel/serial.h"
#include "kernel/spinlock.h"
#include "common/errno.h"
#include "common/dev.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include <stddef.h>

#define MAX_DEV_ENTRIES 32

struct dev_entry {
    char name[32];
    struct inode *ip;
    struct dev_entry *next;
};

static struct dev_entry dev_entries[MAX_DEV_ENTRIES];
static struct dev_entry *dev_list = NULL;
static int dev_count = 0;
static spinlock_t devtmpfs_lock = SPINLOCK_INIT;

void devtmpfs_init(void) {
    spin_lock(&devtmpfs_lock);
    dev_list = NULL;
    dev_count = 0;
    for (int i = 0; i < MAX_DEV_ENTRIES; i++) {
        dev_entries[i].name[0] = '\0';
        dev_entries[i].ip = NULL;
        dev_entries[i].next = NULL;
    }
    spin_unlock(&devtmpfs_lock);
    serial_printf("devtmpfs_init: done\n");
}

struct inode *devtmpfs_lookup(const char *name) {
    spin_lock(&devtmpfs_lock);
    struct dev_entry *e = dev_list;
    while (e) {
        int i;
        for (i = 0; name[i] && e->name[i]; i++) {
            if (name[i] != e->name[i]) break;
        }
        if (name[i] == '\0' && e->name[i] == '\0') {
            spin_unlock(&devtmpfs_lock);
            return e->ip;
        }
        e = e->next;
    }
    spin_unlock(&devtmpfs_lock);
    return NULL;
}

int devtmpfs_create(const char *name, int dev_type, struct dev_ops *ops) {
    if (dev_count >= MAX_DEV_ENTRIES) return -ENOMEM;

    /* Check if already exists */
    if (devtmpfs_lookup(name)) return -EEXIST;

    spin_lock(&devtmpfs_lock);

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_DEV_ENTRIES; i++) {
        if (dev_entries[i].ip == NULL) { slot = i; break; }
    }
    if (slot < 0) {
        spin_unlock(&devtmpfs_lock);
        return -ENOMEM;
    }

    /* Create inode */
    struct inode *ip = inode_create(0, INODE_DEV, 0, 0, 0, 0);
    if (!ip) {
        spin_unlock(&devtmpfs_lock);
        return -ENOMEM;
    }
    ip->i_priv = ops;

    /* Fill entry */
    int i;
    for (i = 0; name[i] && i < 31; i++)
        dev_entries[slot].name[i] = name[i];
    dev_entries[slot].name[i] = '\0';
    dev_entries[slot].ip = ip;
    dev_entries[slot].next = dev_list;
    dev_list = &dev_entries[slot];
    dev_count++;

    spin_unlock(&devtmpfs_lock);
    serial_printf("devtmpfs: created /dev/%s (type=%d)\n", name, dev_type);
    return 0;
}

uint64_t devtmpfs_open(const char *name, int flags) {
    struct inode *ip = devtmpfs_lookup(name);
    if (!ip) return (uint64_t)(-(uint64_t)ENOENT);

    /* Allocate fd */
    proc_t *proc = current_proc;
    int fd = -1;
    for (int j = 3; j < MAX_FD; j++) {
        if (proc->fd_table[j].type == FD_NONE) { fd = j; break; }
    }
    if (fd < 0) return (uint64_t)(-(uint64_t)EMFILE);

    proc->fd_table[fd].type = FD_DEV;
    proc->fd_table[fd].flags = O_RDWR;
    if (ip->i_priv) {
        struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
        proc->fd_table[fd].target_pid = ops->driver_pid;
    }
    return (uint64_t)fd;
}

void devtmpfs_cleanup_pid(pid_t pid) {
    spin_lock(&devtmpfs_lock);
    struct dev_entry **pp = &dev_list;
    while (*pp) {
        struct dev_entry *e = *pp;
        if (e->ip && e->ip->i_priv) {
            struct dev_ops *ops = (struct dev_ops *)e->ip->i_priv;
            if (ops->driver_pid == pid) {
                /* Remove from list */
                *pp = e->next;
                /* Free inode */
                inode_put(e->ip);
                e->ip = NULL;
                e->name[0] = '\0';
                dev_count--;
                continue;
            }
        }
        pp = &e->next;
    }
    spin_unlock(&devtmpfs_lock);
}
