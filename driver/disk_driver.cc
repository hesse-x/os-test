// Disk driver process (user-space)
// Reads requests from DISK_REQ shared page, performs ATA PIO, writes to DISK_RESP
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "common/shm.h"
#include "common/dev.h"

static volatile disk_req_shm  *dreq;
static volatile disk_resp_shm *dresp;
static volatile disk_shm_header *hdr;

// ATA PIO LBA28 I/O ports
#define ATA_DATA     0x1F0
#define ATA_SECTOR   0x1F2
#define ATA_LBA_LO   0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HI   0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_STATUS   0x1F7

#define ATA_CMD_READ  0x20
#define ATA_CMD_WRITE 0x30
#define ATA_BSY       0x80
#define ATA_DRDY      0x40
#define ATA_DRQ       0x08
#define ATA_ERR       0x01

static void ata_read(uint32_t lba, uint32_t count, uint8_t *buf) {
    while (inb(ATA_STATUS) & ATA_BSY);

    outb(ATA_SECTOR, count);
    outb(ATA_LBA_LO,  lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI,  (lba >> 16) & 0xFF);
    outb(ATA_DRIVE, 0xF0 | ((lba >> 24) & 0x0F));
    outb(ATA_STATUS, ATA_CMD_READ);

    uint16_t *dst = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint8_t st;
        while (((st = inb(ATA_STATUS)) & ATA_DRQ) == 0 &&
               (st & ATA_ERR) == 0);
        for (int i = 0; i < 256; i++) {
            *dst++ = inw(ATA_DATA);
        }
    }
}

static void ata_write(uint32_t lba, uint32_t count, const uint8_t *buf) {
    while (inb(ATA_STATUS) & ATA_BSY);

    outb(ATA_SECTOR, count);
    outb(ATA_LBA_LO,  lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI,  (lba >> 16) & 0xFF);
    outb(ATA_DRIVE, 0xF0 | ((lba >> 24) & 0x0F));
    outb(ATA_STATUS, ATA_CMD_WRITE);

    const uint16_t *src = (const uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint8_t st;
        while (((st = inb(ATA_STATUS)) & ATA_DRQ) == 0 &&
               (st & ATA_ERR) == 0);
        for (int i = 0; i < 256; i++) {
            outw(ATA_DATA, *src++);
        }
        // Wait for BSY to clear after each sector
        while (inb(ATA_STATUS) & ATA_BSY);
    }
}

static void handle_request() {
    uint32_t cmd  = dreq->cmd;
    uint32_t lba  = dreq->lba;
    uint32_t cnt  = dreq->count;

    if (cmd == DISK_CMD_READ) {
        // Read directly into response data buffer
        ata_read(lba, cnt, (uint8_t *)dresp->data);
        dresp->status = 0;
        dresp->count  = cnt;
    } else if (cmd == DISK_CMD_WRITE) {
        ata_write(lba, cnt, (const uint8_t *)dreq->data);
        dresp->status = 0;
        dresp->count  = cnt;
    } else {
        dresp->status = 1;
        dresp->count  = 0;
    }
}

extern "C" void _start() {
    // Create shared memory: header(1) + req(2) + resp(2) = 5 pages
    void *shm_ptr = NULL;
    shm_create(5 * 4096, &shm_ptr);
    uint64_t shm_base = (uint64_t)shm_ptr;
    hdr  = (volatile disk_shm_header *)(shm_base + DISK_SHM_HEADER_OFFSET);
    dreq  = (volatile disk_req_shm *)(shm_base + DISK_REQ_OFFSET);
    dresp = (volatile disk_resp_shm *)(shm_base + DISK_RESP_OFFSET);
    hdr->disk_driver_sleeping = 0;
    hdr->fs_driver_sleeping = 0;

    while (1) {
        // Sleep: set flag, wait, clear flag
        hdr->disk_driver_sleeping = 1;
        struct recv_msg msg;
        recv(&msg, NULL, 0, 0);
        hdr->disk_driver_sleeping = 0;

        handle_request();

        // Notify fs_driver only if it's sleeping
        if (hdr->fs_driver_sleeping && hdr->fs_driver_pid > 0) {
            notify(hdr->fs_driver_pid);
        }
    }
}
