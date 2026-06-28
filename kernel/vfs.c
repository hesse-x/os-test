#include "kernel/ahci.h"
#include "kernel/blk_dev.h"
#include "kernel/vfs.h"
#include "kernel/inode.h"
#include "kernel/page_cache.h"
#include "kernel/fat32.h"
#include "kernel/devtmpfs.h"
#include "kernel/display.h"
#include "kernel/pty.h"
#include "kernel/proc.h"
#include "kernel/log.h"
#include "kernel/serial.h"
#include "kernel/sparse.h"
#include "kernel/mem/kasan.h"
#include "kernel/mem/slab.h"
#include "kernel/trap.h"
#include "common/errno.h"
#include "common/dev.h"
#include "common/stat.h"
#include "arch/x64/utils.h"
#include "arch/x64/smp.h"
#include <stddef.h>

void vfs_init(void) {
    inode_init();
    page_cache_init();
    devtmpfs_init();
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
            devtmpfs_create("sda", DEV_BLOCK, &blk_dev_ops);
            break;
        }
    }
    if (rc != 0) {
        printk(LOG_ERROR, "vfs_init: FAT32 init failed on all ports\n");
        return;
    }
}

/* sys_open(path, flags, mode) — syscall 51 */
uint64_t sys_open(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                  uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    const char __user *upath = (const char __user * __force)arg1;
    int flags = (int)arg2;
    /* mode (arg3) ignored for FAT32 */

    /* 1. Copy user path */
    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (uint64_t)(-(uint64_t)EFAULT);

    /* 2. /dev/ prefix — delegate to devtmpfs */
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
        path[3] == 'v' && path[4] == '/') {
        uint64_t dev_ret = devtmpfs_open(current_task, path + 5, flags);
        return dev_ret;
    }

    /* 3. FAT32 path resolution */
    int errno_val = 0;
    struct inode *ip = fat32_open(path, flags, &errno_val);
    if (!ip) {
        return (uint64_t)(int64_t)errno_val;
    }

    /* 4. Allocate fd */
    task_t *proc = current_task;
    int fd = -1;
    for (int j = 3; j < MAX_FD; j++) {
        if (proc->mm->files->fd_table[j].type == FD_NONE) { fd = j; break; }
    }
    if (fd < 0) { inode_put(ip); return (uint64_t)(-(uint64_t)EMFILE); }

    /* 5. Set up fd entry */
    if (ip->type == INODE_DIR) {
        proc->mm->files->fd_table[fd].type = FD_DIR;
        proc->mm->files->fd_table[fd].flags = O_RDONLY;
        proc->mm->files->fd_table[fd].inode = ip;
        proc->mm->files->fd_table[fd].offset = 0;  /* directory scan position */
    } else {
        proc->mm->files->fd_table[fd].type = FD_REGULAR;
        proc->mm->files->fd_table[fd].flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK);
        proc->mm->files->fd_table[fd].inode = ip;
        proc->mm->files->fd_table[fd].offset = 0;
    }
    return (uint64_t)fd;
}

/* sys_stat(path, stat_buf) — syscall 52 */
uint64_t sys_stat(uint64_t arg1, uint64_t arg2, uint64_t _u1,
                  uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const char __user *upath = (const char __user * __force)arg1;
    void __user *stat_buf = (void __user * __force)arg2;

    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (uint64_t)(-(uint64_t)EFAULT);

    uint8_t kstat_buf[256];
    int rc = fat32_stat(path, kstat_buf);
    if (rc != 0) return (uint64_t)(-(uint64_t)(-rc));
    if (copy_to_user(stat_buf, kstat_buf, sizeof(struct kstat))) return (uint64_t)(-(uint64_t)EFAULT);
    return 0;
}

/* sys_mkdir(path, mode) — syscall 53 */
uint64_t sys_mkdir(uint64_t arg1, uint64_t arg2, uint64_t _u1,
                   uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const char __user *upath = (const char __user * __force)arg1;

    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (uint64_t)(-(uint64_t)EFAULT);

    int rc = fat32_mkdir(path);
    if (rc != 0) return (uint64_t)(-(uint64_t)(-rc));
    return 0;
}

/* sys_unlink(path) — syscall 54 */
uint64_t sys_unlink(uint64_t arg1, uint64_t _u1, uint64_t _u2,
                    uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    const char __user *upath = (const char __user * __force)arg1;

    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (uint64_t)(-(uint64_t)EFAULT);

    int rc = fat32_unlink(path);
    if (rc != 0) return (uint64_t)(-(uint64_t)(-rc));
    return 0;
}

/* sys_rmdir(path) — syscall 55 */
uint64_t sys_rmdir(uint64_t arg1, uint64_t _u1, uint64_t _u2,
                   uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    const char __user *upath = (const char __user * __force)arg1;

    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    if (strncpy_from_user(path, upath, 256) < 0) return (uint64_t)(-(uint64_t)EFAULT);

    int rc = fat32_rmdir(path);
    if (rc != 0) return (uint64_t)(-(uint64_t)(-rc));
    return 0;
}

/* sys_dev_create(name, dev_type) — syscall 55
 * Kernel auto-fills driver_pid=current_task->pid, all callbacks NULL (user-space driver) */
uint64_t sys_dev_create(uint64_t arg1, uint64_t arg2, uint64_t _u1,
                        uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const char __user *uname = (const char __user * __force)arg1;
    int dev_type = (int)arg2;

    if (!uname) return (uint64_t)(-(uint64_t)EFAULT);
    char name[32];
    if (strncpy_from_user(name, uname, 32) < 0) return (uint64_t)(-(uint64_t)EFAULT);

    struct dev_ops *kops = kmalloc(sizeof(struct dev_ops));
    if (!kops) return (uint64_t)(-(uint64_t)ENOMEM);
    __memset(kops, 0, sizeof(struct dev_ops));

    // Force driver_pid to current process — user-space can't set this
    kops->driver_pid = current_task->pid;
    kops->device_type = dev_type;
    // All callbacks remain NULL for user-space drivers (IPC proxy handles requests)

    int rc = devtmpfs_create(name, dev_type, kops);
    if (rc != 0) {
        kfree(kops);
        return (uint64_t)(-(uint64_t)(-rc));
    }

    // isr_driver_pid is populated inside devtmpfs_create for user-space drivers
    // (no separate register_dev call needed)

    return 0;
}

/* sys_getdents(fd, buf, len) — syscall 58
 * Read directory entries into user buffer.
 * fd must be FD_DIR. Returns bytes written, 0 on EOF, or negative errno. */
uint64_t sys_getdents(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                      uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    int fd = (int)arg1;
    void __user *buf = (void __user * __force)arg2;
    size_t len = (size_t)arg3;

    if (fd < 0 || fd >= MAX_FD) return (uint64_t)-EINVAL;
    if (current_task->mm->files->fd_table[fd].type != FD_DIR) return (uint64_t)-ENOTDIR;

    struct inode *ip = current_task->mm->files->fd_table[fd].inode;
    if (!ip) return (uint64_t)-EBADF;

    void *kbuf = kmalloc(len);
    if (!kbuf) return (uint64_t)-ENOMEM;
    int ret = fat32_getdents(ip->start_cluster, &current_task->mm->files->fd_table[fd].offset, kbuf, len);
    if (ret < 0) { kfree(kbuf); return (uint64_t)(-(uint64_t)(-ret)); }
    if (copy_to_user(buf, kbuf, ret)) { kfree(kbuf); return (uint64_t)(-(uint64_t)EFAULT); }
    kfree(kbuf);
    return (uint64_t)ret;
}
