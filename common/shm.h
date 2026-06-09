#ifndef COMMON_SHM_H
#define COMMON_SHM_H

#include <stdint.h>

// Shared memory virtual addresses (fixed, mapped into all user processes)
#define KBD_SHM_ADDR   0x500000
#define DISK_REQ_ADDR  0x501000
#define DISK_RESP_ADDR 0x502000

// Keyboard shared page (driver -> consumer)
struct kbd_shm {
    uint32_t head;       // write position (ring buffer)
    uint32_t tail;       // read position
    uint8_t  data[4088]; // key event ring buffer
};

// Disk request shared page (consumer -> driver)
struct disk_req_shm {
    uint32_t cmd;        // READ=0, WRITE=1
    uint32_t lba;
    uint32_t count;      // sector count
    uint8_t  data[4076]; // write data
};

// Disk response shared page (driver -> consumer)
struct disk_resp_shm {
    uint32_t status;     // 0=success
    uint32_t count;      // actual sectors transferred
    uint8_t  data[4088]; // read data
};

#endif
