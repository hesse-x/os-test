#include "kernel/devtmpfs.h"
#include "kernel/inode.h"
#include "kernel/proc.h"
#include "kernel/serial.h"
#include "kernel/spinlock.h"
#include "kernel/mem/slab.h"
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

// ISR-facing driver PID lookup (replaces dev_table for ISR wake)
static pid_t isr_driver_pid[DEV_TYPE_MAX];

void devtmpfs_init(void) {
    spin_lock(&devtmpfs_lock);
    dev_list = NULL;
    dev_count = 0;
    for (int i = 0; i < DEV_TYPE_MAX; i++)
        isr_driver_pid[i] = 0;
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

    // Populate ISR driver PID table for user-space drivers
    if (ops->driver_pid > 0)
        isr_driver_pid[ops->device_type] = ops->driver_pid;

    spin_unlock(&devtmpfs_lock);
    serial_printf("devtmpfs: created /dev/%s (type=%d)\n", name, dev_type);
    return 0;
}

uint64_t devtmpfs_open(struct proc_t *proc, const char *name, int flags) {
    serial_printf("devtmpfs_open: name='%s' pid=%d\n", name, proc->pid);
    struct inode *ip = devtmpfs_lookup(name);
    if (!ip) return (uint64_t)(-(uint64_t)ENOENT);

    /* Allocate fd */
    int fd = -1;
    for (int j = 3; j < MAX_FD; j++) {
        if (proc->fd_table[j].type == FD_NONE) { fd = j; break; }
    }
    if (fd < 0) return (uint64_t)(-(uint64_t)EMFILE);

    proc->fd_table[fd].type = FD_DEV;
    proc->fd_table[fd].flags = flags;
    proc->fd_table[fd].inode = ip;
    inode_get(ip);
    if (ip->i_priv) {
        struct dev_ops *ops = (struct dev_ops *)ip->i_priv;
        proc->fd_table[fd].target_pid = ops->driver_pid;
        // Kernel device: call open callback
        if (ops->driver_pid == 0 && ops->open) {
            int rc = ops->open(proc, fd);
            if (rc < 0) {
                // Open failed: undo fd allocation
                inode_put(ip);
                __memset(&proc->fd_table[fd], 0, sizeof(struct file));
                proc->fd_table[fd].type = FD_NONE;
                return (uint64_t)(-(uint64_t)(-rc));
            }
        }
    }
    serial_printf("devtmpfs_open: '%s' allocated fd=%d driver_pid=%d\n", name, fd, ip->i_priv ? ((struct dev_ops *)ip->i_priv)->driver_pid : -1);
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
                /* Clear ISR driver PID table entry */
                if (ops->driver_pid > 0)
                    isr_driver_pid[ops->device_type] = 0;
                /* Free kmalloc'd dev_ops (user-space driver) */
                if (ops->driver_pid > 0) kfree(ops);
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

pid_t isr_lookup_driver(uint32_t dev_type) {
    if (dev_type >= DEV_TYPE_MAX) return 0;
    return isr_driver_pid[dev_type];
}
