#include "kernel/driver/ahci.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/bsd/vfs.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/bsd/fat32.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/display.h"
#include "kernel/bsd/pty.h"
#include "kernel/xcore/xtask.h"
#include "kernel/bsd/types.h"
#include "kernel/bsd/proc.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/log.h"
#include "kernel/driver/serial.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/trap.h"
#include "xos/errno.h"
#include "xos/stat.h"
#include "arch/x64/utils.h"
#include "arch/x64/smp.h"
#include <stddef.h>

void vfs_init(void) {
    // inode_init, page_cache_init, devtmpfs_init are called in kernel_main before driver_init
    display_dev_register();
    serial_dev_register();
    pty_init();

    /* Try FAT32 on each AHCI port (disk.img may be on a different port
       than boot.img, just like ELF loading in kernel_main). */
    int try_ports[] = { 0, 1, 2, 3, 4, 5 };
    int rc = -1;
    for (int pi = 0; pi < 6; pi++) {
        if (ahci_set_active_port(try_ports[pi]) != 0) continue;
        rc = fat32_init();
        if (rc == 0) {
            printk(LOG_INFO, "vfs_init: FAT32 mounted on port %d\n", try_ports[pi]);
            devtmpfs_create("sda", &blk_dev_ops, NULL);
            break;
        }
    }
    if (rc != 0) {
        printk(LOG_ERROR, "vfs_init: FAT32 init failed on all ports\n");
        return;
    }
}

/* sys_open(path, flags, mode) — SYS_OPEN */
int64_t sys_open(int64_t arg1, int64_t arg2, int64_t arg3,
                  int64_t _u1, int64_t _u2, int64_t _u3) {
    const char __user *upath = (const char __user * __force)arg1;
    int flags = (int)arg2;
    /* mode (arg3) ignored for FAT32 */

    /* 1. Copy user path */
    if (!upath) return (int64_t)-EFAULT;
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (int64_t)-EFAULT;

    /* 2. /dev/ prefix — delegate to devtmpfs */
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
        path[3] == 'v' && path[4] == '/') {
        printk(LOG_DEBUG, "sys_open: pid=%d path=%s\n", current_task->pid, path);
        int64_t dev_ret = devtmpfs_open(current_task, path + 5, flags);
        printk(LOG_DEBUG, "sys_open: devtmpfs_open returned %lld\n", (long long)dev_ret);
        return dev_ret;
    }

    /* 3. FAT32 path resolution */
    int errno_val = 0;
    struct inode *ip = fat32_open(path, flags, &errno_val);
    if (!ip) {
        return errno_val;
    }

    /* 4. Allocate fd (under fd_lock) */
    xtask_t *proc = current_task;
    files_t *files = proc->proc->files;
    spinlock_t *fdlk = &files->fd_lock;
    spin_lock(fdlk);
    int fd = alloc_fd(files, 3);
    if (fd < 0) { spin_unlock(fdlk); inode_put(ip); return (int64_t)-EMFILE; }

    /* 5. Allocate struct file */
    struct file *f = (struct file *)kmalloc(sizeof(struct file));
    if (!f) { spin_unlock(fdlk); inode_put(ip); return (int64_t)-ENOMEM; }
    __memset(f, 0, sizeof(*f));
    refcount_set(&f->f_count, 1);

    /* 6. Set up fd entry */
    if (ip->type == INODE_DIR) {
        f->type = FD_DIR;
        f->flags = O_RDONLY;
        f->inode = ip;
        f->offset = 0;  /* directory scan position */
    } else {
        f->type = FD_REGULAR;
        f->flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK);
        f->inode = ip;
        f->offset = 0;
    }
    fd_install(files, fd, f);
    spin_unlock(fdlk);
    return (int64_t)fd;
}

/* sys_stat(path, stat_buf) — SYS_STAT */
int64_t sys_stat(int64_t arg1, int64_t arg2, int64_t _u1,
                  int64_t _u2, int64_t _u3, int64_t _u4) {
    const char __user *upath = (const char __user * __force)arg1;
    void __user *stat_buf = (void __user * __force)arg2;

    if (!upath) return (int64_t)-EFAULT;
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (int64_t)-EFAULT;

    uint8_t kstat_buf[256];
    int rc = fat32_stat(path, kstat_buf);
    if (rc != 0) return rc;
    if (copy_to_user(stat_buf, kstat_buf, sizeof(struct kstat))) return (int64_t)-EFAULT;
    return 0;
}

/* sys_mkdir(path, mode) — SYS_MKDIR */
int64_t sys_mkdir(int64_t arg1, int64_t arg2, int64_t _u1,
                   int64_t _u2, int64_t _u3, int64_t _u4) {
    const char __user *upath = (const char __user * __force)arg1;

    if (!upath) return (int64_t)-EFAULT;
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (int64_t)-EFAULT;

    int rc = fat32_mkdir(path);
    if (rc != 0) return rc;
    return 0;
}

/* sys_unlink(path) — SYS_UNLINK */
int64_t sys_unlink(int64_t arg1, int64_t _u1, int64_t _u2,
                    int64_t _u3, int64_t _u4, int64_t _u5) {
    const char __user *upath = (const char __user * __force)arg1;

    if (!upath) return (int64_t)-EFAULT;
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (int64_t)-EFAULT;

    int rc = fat32_unlink(path);
    if (rc != 0) return rc;
    return 0;
}

/* sys_rmdir(path) — SYS_RMDIR */
int64_t sys_rmdir(int64_t arg1, int64_t _u1, int64_t _u2,
                   int64_t _u3, int64_t _u4, int64_t _u5) {
    const char __user *upath = (const char __user * __force)arg1;

    if (!upath) return (int64_t)-EFAULT;
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (int64_t)-EFAULT;

    int rc = fat32_rmdir(path);
    if (rc != 0) return rc;
    return 0;
}

/* sys_dev_create(name, shm_fd) — SYS_DEV_CREATE
 * Kernel auto-fills driver_pid=current_task->pid, is_block=false, callbacks NULL (user-space driver) */
int64_t sys_dev_create(int64_t arg1, int64_t arg2, int64_t arg3,
                        int64_t _u1, int64_t _u2, int64_t _u3) {
    const char __user *uname = (const char __user * __force)arg1;
    int shm_fd = (int)arg2;
    struct shm *dev_shm = NULL;

    if (!uname) return (int64_t)-EFAULT;
    char name[32];
    if (strncpy_from_user(name, uname, 32) < 0) return (int64_t)-EFAULT;

    struct dev_ops *kops = kmalloc(sizeof(struct dev_ops));
    if (!kops) return (int64_t)-ENOMEM;
    __memset(kops, 0, sizeof(struct dev_ops));

    // Force driver_pid to current process — user-space can't set this
    kops->driver_pid = current_task->pid;
    kops->is_block = false;
    // All callbacks remain NULL for user-space drivers (IPC proxy handles requests)

    // Resolve shm_fd to struct shm* (if provided)
    if (shm_fd >= 0) {
        xtask_t *proc = current_task;
        if (shm_fd >= MAX_FD) {
            kfree(kops);
            return (int64_t)-EBADF;
        }
        spinlock_t *fdlk = &proc->proc->files->fd_lock;
        spin_lock(fdlk);
        struct file *sf = proc->proc->files->fd_table[shm_fd];
        if (!sf || sf->type != FD_SHM) {
            spin_unlock(fdlk);
            kfree(kops);
            return (int64_t)-EINVAL;
        }
        dev_shm = sf->shm;
        spin_unlock(fdlk);
    }

    int rc = devtmpfs_create(name, kops, dev_shm);
    if (rc != 0) {
        kfree(kops);
        return rc;
    }

    return 0;
}

/* sys_getdents(fd, buf, len) — SYS_GETDENTS
 * Read directory entries into user buffer.
 * fd must be FD_DIR. Returns bytes written, 0 on EOF, or negative errno. */
int64_t sys_getdents(int64_t arg1, int64_t arg2, int64_t arg3,
                      int64_t _u1, int64_t _u2, int64_t _u3) {
    int fd = (int)arg1;
    void __user *buf = (void __user * __force)arg2;
    size_t len = (size_t)arg3;

    if (fd < 0 || fd >= MAX_FD) return (int64_t)-EINVAL;
    if (len == 0 || len > 4096) return (int64_t)-EINVAL;

    rcu_read_lock();
    struct file *f = fd_lookup(current_proc->files, fd);
    if (!f || f->type != FD_DIR) { rcu_read_unlock(); return (int64_t)-ENOTDIR; }
    file_get(f);
    rcu_read_unlock();

    struct inode *ip = f->inode;
    if (!ip) { file_put(f); return (int64_t)-EBADF; }

    void *kbuf = kmalloc(len);
    if (!kbuf) { file_put(f); return (int64_t)-ENOMEM; }
    int ret = fat32_getdents(ip->start_cluster, &f->offset, kbuf, len);
    if (ret < 0) { kfree(kbuf); file_put(f); return ret; }
    if (copy_to_user(buf, kbuf, ret)) { kfree(kbuf); file_put(f); return (int64_t)-EFAULT; }
    kfree(kbuf);
    file_put(f);
    return (int64_t)ret;
}
