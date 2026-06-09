// Disk driver process (user-space)
// Reads requests from DISK_REQ shared page, performs ATA PIO, writes to DISK_RESP
#include <stdint.h>
#include "arch/x64/utils.h"
#include "common/shm.h"

static volatile disk_req_shm  *req  = (volatile disk_req_shm  *)DISK_REQ_ADDR;
static volatile disk_resp_shm *resp = (volatile disk_resp_shm *)DISK_RESP_ADDR;

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

#define DISK_CMD_READ  0
#define DISK_CMD_WRITE 1

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

static void handle_request() {
    uint32_t cmd  = req->cmd;
    uint32_t lba  = req->lba;
    uint32_t cnt  = req->count;

    if (cmd == DISK_CMD_READ) {
        // Read directly into response data buffer
        ata_read(lba, cnt, (uint8_t *)resp->data);
        resp->status = 0;
        resp->count  = cnt;
    } else {
        // WRITE not implemented yet
        resp->status = 1;
        resp->count  = 0;
    }
}

extern "C" void _start() {
    // Shell PID = our PID + 2 (PID2=disk_driver, PID3=kbd_driver, PID4=shell)
    int32_t my_pid = (int32_t)sys_getpid();
    int32_t shell_pid = my_pid + 2;

    while (1) {
        // Wait for request
        sys_wait();

        handle_request();

        sys_notify(shell_pid);
    }
}
