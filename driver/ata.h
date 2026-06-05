#ifndef DRIVER_ATA_H
#define DRIVER_ATA_H

#include <stdint.h>

void ata_read_lba(uint32_t lba, uint32_t count, void *buf);

#endif // DRIVER_ATA_H
