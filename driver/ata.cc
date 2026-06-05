#include "driver/ata.h"
#include "arch/x64/utils.h"

// ATA PIO LBA28 I/O ports
#define ATA_DATA     0x1F0
#define ATA_SECTOR   0x1F2
#define ATA_LBA_LO   0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HI   0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_STATUS   0x1F7

#define ATA_CMD_READ 0x20
#define ATA_BSY      0x80
#define ATA_DRDY     0x40
#define ATA_DRQ      0x08
#define ATA_ERR      0x01

void ata_read_lba(uint32_t lba, uint32_t count, void *buf) {
    // 1. Wait for BSY to clear
    while (inb(ATA_STATUS) & ATA_BSY);

    // 2. Write parameters
    outb(ATA_SECTOR, count);
    outb(ATA_LBA_LO,  lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI,  (lba >> 16) & 0xFF);
    outb(ATA_DRIVE, 0xF0 | ((lba >> 24) & 0x0F));

    // 3. Send read command
    outb(ATA_STATUS, ATA_CMD_READ);

    uint16_t *dst = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        // 4. Wait for DRQ or ERR
        uint8_t st;
        while (((st = inb(ATA_STATUS)) & ATA_DRQ) == 0 &&
               (st & ATA_ERR) == 0);

        // 5. Read 256 words = 512 bytes
        for (int i = 0; i < 256; i++) {
            *dst++ = inw(ATA_DATA);
        }
    }
}
