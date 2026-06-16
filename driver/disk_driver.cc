// Disk driver process (user-space)
// Reads requests from DISK_REQ shared page, performs ATA PIO, writes to DISK_RESP
// DMA support deferred until PCI subsystem is ready (see doc/design/file_system.md §5.10)
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <sys/device.h>
#include <sys/serial.h>
#include "common/shm.h"
#include "common/dev.h"
#include "common/syscall.h"

static volatile disk_req_shm  *dreq;
static volatile disk_resp_shm *dresp;
static volatile disk_shm_header *hdr;

// ATA LBA28 I/O ports
#define ATA_DATA     0x1F0
#define ATA_FEATURES 0x1F1
#define ATA_SECTOR   0x1F2
#define ATA_LBA_LO   0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HI   0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_STATUS   0x1F7
#define ATA_ALT_STATUS 0x3F6

#define ATA_CMD_READ  0x20
#define ATA_CMD_WRITE 0x30
#define ATA_BSY       0x80
#define ATA_DRDY      0x40
#define ATA_DRQ       0x08
#define ATA_ERR       0x01

static void ata_wait_bsy() {
    int timeout = 1000000;
    while (timeout-- > 0) {
        if ((inb(ATA_ALT_STATUS) & ATA_BSY) == 0) return;
    }
}

static void ata_pio_read(uint32_t lba, uint32_t count, uint8_t *buf) {
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

static void ata_pio_write(uint32_t lba, uint32_t count, const uint8_t *buf) {
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
    }
    while (inb(ATA_STATUS) & ATA_BSY);
}

static void handle_request() {
    uint32_t cmd  = dreq->cmd;
    uint32_t lba  = dreq->lba;
    uint32_t cnt  = dreq->count;

    if (cmd == DISK_CMD_READ) {
        int rc = 0;
        uint32_t done = 0;
        while (done < cnt) {
            uint32_t chunk = cnt - done;
            if (chunk > 32) chunk = 32;
            ata_pio_read(lba + done, chunk, (uint8_t *)dresp->data + done * 512);
            done += chunk;
        }
        dresp->status = 0;
        dresp->count  = done;
    } else if (cmd == DISK_CMD_WRITE) {
        uint32_t done = 0;
        while (done < cnt) {
            uint32_t chunk = cnt - done;
            if (chunk > 32) chunk = 32;
            ata_pio_write(lba + done, chunk, (const uint8_t *)dreq->data + done * 512);
            done += chunk;
        }
        dresp->status = 0;
        dresp->count  = done;
    } else {
        dresp->status = 1;
        dresp->count  = 0;
    }
}

extern "C" void _start() {
    int my_pid = getpid();
    serial_write("disk_driver: started\n", 21);

    // Enable I/O ports for ATA registers
    ioperm(0x1F0, 8, 1);   // ATA command block registers
    ioperm(0x3F6, 2, 1);   // ATA control block registers

    // Register as disk device
    device_register(my_pid, DEV_DISK);
    serial_write("disk_driver: registered DEV_DISK\n", 33);

    // Create shared memory: header(1) + req(2) + resp(5) = 8 pages
    void *shm_ptr = NULL;
    shm_create(8 * 4096, &shm_ptr);
    uint64_t shm_base = (uint64_t)shm_ptr;
    hdr  = (volatile disk_shm_header *)(shm_base + DISK_SHM_HEADER_OFFSET);
    dreq  = (volatile disk_req_shm *)(shm_base + DISK_REQ_OFFSET);
    dresp = (volatile disk_resp_shm *)(shm_base + DISK_RESP_OFFSET);
    hdr->disk_driver_sleeping = 0;
    hdr->fs_driver_sleeping = 0;
    serial_write("disk_driver: entering main loop\n", 32);

    while (1) {
        hdr->disk_driver_sleeping = 1;
        struct recv_msg msg;
        recv(&msg, NULL, 0, 0);
        hdr->disk_driver_sleeping = 0;

        handle_request();

        if (hdr->fs_driver_sleeping && hdr->fs_driver_pid > 0) {
            notify(hdr->fs_driver_pid);
        }
    }
}
