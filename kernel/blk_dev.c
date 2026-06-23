#include "kernel/blk_dev.h"
#include "kernel/ahci.h"
#include "kernel/spinlock.h"
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
