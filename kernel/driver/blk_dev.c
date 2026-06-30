#include "kernel/driver/blk_dev.h"
#include "kernel/driver/ahci.h"
#include "kernel/xcore/spinlock.h"
#include "arch/x64/utils.h"

int blk_read(uint32_t lba, uint32_t count, void *buf) {
    uint64_t flags;
    spin_lock_irqsave(&ahci_lock, &flags);
    int rc = ahci_read_lba(lba, count, buf);
    spin_unlock_irqrestore(&ahci_lock, flags);
    return rc;
}

int blk_write(uint32_t lba, uint32_t count, const void *buf) {
    uint64_t flags;
    spin_lock_irqsave(&ahci_lock, &flags);
    int rc = ahci_write_lba(lba, count, buf);
    spin_unlock_irqrestore(&ahci_lock, flags);
    return rc;
}

int blk_read_sector(uint32_t lba, void *buf) {
    return blk_read(lba, 1, buf);
}

#include "kernel/bsd/devtmpfs.h"
#include "kernel/xcore/xtask.h"
#include "kernel/driver/bsd_types.h"
#include "common/errno.h"

static int blk_dev_open(xtask_t *proc, int fd) {
    return 0;
}

static int blk_dev_close(xtask_t *proc, int fd) {
    return 0;
}

static ssize_t blk_dev_read(xtask_t *proc, int fd, void *buf, size_t count) {
    struct file *f = fd_lookup(proc->proc->files, fd);
    if (!f) return -EBADF;
    uint64_t off = f->offset;

    if (off % 512 != 0 || count % 512 != 0)
        return -EINVAL;

    uint32_t lba = (uint32_t)(off / 512);
    uint32_t nsec = (uint32_t)(count / 512);

    if (nsec > AHCI_MAX_SECTORS)
        nsec = AHCI_MAX_SECTORS;

    int ret = blk_read(lba, nsec, buf);
    if (ret < 0)
        return ret;

    size_t done = nsec * 512;
    f->offset += done;
    return (ssize_t)done;
}

static ssize_t blk_dev_write(xtask_t *proc, int fd, const void *buf, size_t count) {
    struct file *f = fd_lookup(proc->proc->files, fd);
    if (!f) return -EBADF;
    uint64_t off = f->offset;

    if (off % 512 != 0 || count % 512 != 0)
        return -EINVAL;

    uint32_t lba = (uint32_t)(off / 512);
    uint32_t nsec = (uint32_t)(count / 512);

    if (nsec > AHCI_MAX_SECTORS)
        nsec = AHCI_MAX_SECTORS;

    int ret = blk_write(lba, nsec, buf);
    if (ret < 0)
        return ret;

    size_t done = nsec * 512;
    f->offset += done;
    return (ssize_t)done;
}

struct dev_ops blk_dev_ops = {
    .driver_pid  = 0,
    .is_block    = true,
    .open        = blk_dev_open,
    .close       = blk_dev_close,
    .read        = blk_dev_read,
    .write       = blk_dev_write,
    .ioctl       = NULL,
    .mmap        = NULL,
    .poll        = NULL,
};
