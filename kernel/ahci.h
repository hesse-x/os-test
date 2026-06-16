#ifndef KERNEL_AHCI_H
#define KERNEL_AHCI_H

#include <stdint.h>
#include "kernel/spinlock.h"

#define AHCI_MAX_SECTORS 128  // 64KB bounce buffer / 512 bytes per sector

void ahci_init();
int ahci_read_lba(uint32_t lba, uint32_t count, void *buf);
int ahci_write_lba(uint32_t lba, uint32_t count, const void *buf);

extern spinlock_t ahci_lock;

#endif // KERNEL_AHCI_H
