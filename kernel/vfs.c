#include "kernel/vfs.h"
#include "kernel/inode.h"
#include "kernel/page_cache.h"
#include "kernel/fat32.h"
#include "kernel/devtmpfs.h"
#include "kernel/proc.h"
#include "kernel/serial.h"
#include "common/errno.h"
#include "arch/x64/utils.h"
#include "arch/x64/smp.h"
#include <stddef.h>

void vfs_init(void) {
    inode_init();
    page_cache_init();
    int rc = fat32_init();
    if (rc != 0) {
        serial_printf("vfs_init: FAT32 init failed (rc=%d)\n", rc);
        return;
    }
    devtmpfs_init();
    serial_printf("vfs_init: done, FAT32 mounted\n");
}

/* sys_open(path, flags, mode) — syscall 51 */
uint64_t sys_open(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                  uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    const char *upath = (const char *)arg1;
    int flags = (int)arg2;
    /* mode (arg3) ignored for FAT32 */

    /* 1. Copy user path */
    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    int i;
    for (i = 0; i < 255; i++) {
        char c = upath[i];
        if (c == '\0') break;
        path[i] = c;
    }
    path[i] = '\0';

    /* 2. /dev/ prefix — delegate to devtmpfs */
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
        path[3] == 'v' && path[4] == '/')
        return devtmpfs_open(path + 5, flags);

    /* 3. FAT32 path resolution */
    int errno_val = 0;
    struct inode *ip = fat32_open(path, flags, &errno_val);
    if (!ip) return (uint64_t)(-(uint64_t)errno_val);

    /* 4. Allocate fd */
    proc_t *proc = current_proc;
    int fd = -1;
    for (int j = 3; j < MAX_FD; j++) {
        if (proc->fd_table[j].type == FD_NONE) { fd = j; break; }
    }
    if (fd < 0) { inode_put(ip); return (uint64_t)(-(uint64_t)EMFILE); }

    /* 5. Set up fd entry */
    proc->fd_table[fd].type = FD_REGULAR;
    proc->fd_table[fd].flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_NONBLOCK);
    proc->fd_table[fd].inode = ip;
    proc->fd_table[fd].offset = 0;
    return (uint64_t)fd;
}

/* sys_stat(path, stat_buf) — syscall 52 */
uint64_t sys_stat(uint64_t arg1, uint64_t arg2, uint64_t _u1,
                  uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const char *upath = (const char *)arg1;
    void *stat_buf = (void *)arg2;

    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    int i;
    for (i = 0; i < 255; i++) {
        char c = upath[i];
        if (c == '\0') break;
        path[i] = c;
    }
    path[i] = '\0';

    int rc = fat32_stat(path, stat_buf);
    if (rc != 0) return (uint64_t)(-(uint64_t)(-rc));
    return 0;
}

/* sys_mkdir(path, mode) — syscall 53 */
uint64_t sys_mkdir(uint64_t arg1, uint64_t arg2, uint64_t _u1,
                   uint64_t _u2, uint64_t _u3, uint64_t _u4) {
    const char *upath = (const char *)arg1;

    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    int i;
    for (i = 0; i < 255; i++) {
        char c = upath[i];
        if (c == '\0') break;
        path[i] = c;
    }
    path[i] = '\0';

    int rc = fat32_mkdir(path);
    if (rc != 0) return (uint64_t)(-(uint64_t)(-rc));
    return 0;
}

/* sys_unlink(path) — syscall 54 */
uint64_t sys_unlink(uint64_t arg1, uint64_t _u1, uint64_t _u2,
                    uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    const char *upath = (const char *)arg1;

    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    int i;
    for (i = 0; i < 255; i++) {
        char c = upath[i];
        if (c == '\0') break;
        path[i] = c;
    }
    path[i] = '\0';

    int rc = fat32_unlink(path);
    if (rc != 0) return (uint64_t)(-(uint64_t)(-rc));
    return 0;
}

/* sys_rmdir(path) — syscall 55 */
uint64_t sys_rmdir(uint64_t arg1, uint64_t _u1, uint64_t _u2,
                   uint64_t _u3, uint64_t _u4, uint64_t _u5) {
    const char *upath = (const char *)arg1;

    if (!upath) return (uint64_t)(-(uint64_t)EFAULT);
    char path[256];
    int i;
    for (i = 0; i < 255; i++) {
        char c = upath[i];
        if (c == '\0') break;
        path[i] = c;
    }
    path[i] = '\0';

    int rc = fat32_rmdir(path);
    if (rc != 0) return (uint64_t)(-(uint64_t)(-rc));
    return 0;
}

/* sys_dev_create(name, dev_type, ops_ptr) — syscall 56 */
uint64_t sys_dev_create(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                        uint64_t _u1, uint64_t _u2, uint64_t _u3) {
    const char *uname = (const char *)arg1;
    int dev_type = (int)arg2;
    struct dev_ops *ops = (struct dev_ops *)arg3;

    if (!uname) return (uint64_t)(-(uint64_t)EFAULT);
    char name[32];
    int i;
    for (i = 0; i < 31; i++) {
        char c = uname[i];
        if (c == '\0') break;
        name[i] = c;
    }
    name[i] = '\0';

    int rc = devtmpfs_create(name, dev_type, ops);
    if (rc != 0) return (uint64_t)(-(uint64_t)(-rc));
    return 0;
}
