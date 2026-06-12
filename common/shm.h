#ifndef COMMON_SHM_H
#define COMMON_SHM_H

#include <stdint.h>

// Shared memory virtual addresses (fixed, mapped into all user processes)
// Layout: 7 pages (0x501000-0x507FFF)
//   0x501000  disk_req_shm   (2 pages, 0x501000-0x502FFF)
//   0x503000  disk_resp_shm  (2 pages, 0x503000-0x504FFF)
//   0x505000  fs_req_shm     (1 page)
//   0x506000  fs_resp_shm    (2 pages, 0x506000-0x507FFF)
// KBD/KMS shared memory is now dynamic (sys_shm_create/sys_shm_attach)
#define DISK_REQ_ADDR   0x501000
#define DISK_REQ_ADDR2  0x502000   // second page of disk_req (expanded to 2 pages)
#define DISK_RESP_ADDR  0x503000
#define DISK_RESP_ADDR2 0x504000   // second page of disk_resp
#define FS_REQ_ADDR     0x505000
#define FS_RESP_ADDR    0x506000
#define FS_RESP_ADDR2   0x507000   // second page of fs_resp

// Disk request shared page (fs_driver -> disk_driver), 2 pages
struct disk_req_shm {
    uint32_t cmd;        // READ=0, WRITE=1
    uint32_t lba;
    uint32_t count;      // sector count
    uint8_t  data[8180]; // write data (2 pages - 12 bytes header)
};

// Disk response shared page (disk_driver -> fs_driver), 2 pages
struct disk_resp_shm {
    uint32_t status;     // 0=success
    uint32_t count;      // actual sectors transferred
    uint8_t  data[8180]; // read data (2 pages - 8 bytes header)
};

// FS request shared page (shell -> fs_driver)
struct fs_req_shm {
    uint32_t cmd;        // FS_CMD_*
    uint32_t client_pid; // requester PID
    char     path[256];  // absolute path
    uint32_t fd;         // for read/close
    uint32_t offset;     // for read
    uint32_t count;      // for read
    uint32_t lba;        // for raw_read
};

// FS response shared page (fs_driver -> shell), 2 pages
struct fs_resp_shm {
    uint32_t status;     // 0=success, nonzero=error
    uint32_t fd;         // open returns fd
    uint32_t count;      // bytes read/returned
    uint32_t total;      // file size (open) or entry count (readdir)
    uint8_t  data[8176]; // data area (2 pages - 16 bytes header)
};

// Directory entry (readdir)
struct fs_dirent {
    char     name[28];   // 8.3 short name
    uint32_t size;       // file size
    uint32_t date;       // modification date
    uint8_t  attr;       // FAT attributes (dir/readonly etc)
};

// FS commands
#define FS_CMD_READDIR   0
#define FS_CMD_OPEN      1
#define FS_CMD_READ      2
#define FS_CMD_CLOSE     3
#define FS_CMD_RAW_READ  4
#define FS_CMD_CREATE    5   // touch: create empty file or update timestamp
#define FS_CMD_MKDIR     6   // mkdir: create directory

// KMS framebuffer info (returned by sys_fb_info)
struct kms_fb_info {
    uint32_t width;      // pixel width
    uint32_t height;     // pixel height
    uint32_t pitch;      // bytes per line
    uint32_t bpp;        // bits per pixel
    uint64_t fb_vaddr;   // framebuffer virtual address in KMS process (0x700000)
    uint64_t fb_size;    // framebuffer size in bytes
    uint64_t fb_phys;    // framebuffer physical address (for KMS driver reference)
};

// KMS request commands (used in kms_msg)
#define KMS_CMD_PUTC        0   // arg1=char, arg2=fg color
#define KMS_CMD_CLEAR       1   // no args
#define KMS_CMD_SCROLL      2   // no args, scroll up one line
#define KMS_CMD_CURSOR_MOVE 3   // arg1=x, arg2=y

// ===================== Driver shared page layout =====================
// One 4K page created by kbd_driver via sys_shm_create(4096),
// attached by kms_driver and shell via sys_shm_attach(KBD_DRIVER_PID).
//
// Offset 0:   driver_shm_header (8 bytes)
// Offset 8:   kbd ring buffer (head + tail + msgs[8] = 72 bytes, padded to 128)
// Offset 128: kms ring buffer (head + tail + msgs[240] = 3848 bytes)

#define KBD_RING_OFFSET  0
#define KMS_RING_OFFSET  128
#define KMS_RING_SIZE    240

struct driver_shm_header {
    uint8_t kbd_sleeping;      // 1 = kbd_driver is sleeping
    uint8_t consumer_sleeping;  // 1 = consumer (shell) is sleeping
    uint8_t kms_sleeping;      // 1 = kms_driver is sleeping
    uint8_t reserved[5];
};

struct kbd_msg {
    uint8_t type;       // 1=key event
    uint8_t ch;         // ASCII character
    uint8_t reserved[6];
};

// KBD ring buffer at offset 0:
//   driver_shm_header (8B) + head(4B) + tail(4B) + kbd_msg[8] (64B) + padding (48B) = 128B
struct kbd_ring {
    struct driver_shm_header header;  // 8 bytes at offset 0
    uint32_t head;                    // write position (0..7)
    uint32_t tail;                    // read position (0..7)
    struct kbd_msg msgs[8];           // 8 slots × 8 bytes = 64 bytes
    uint8_t padding[48];             // pad to 128 bytes total
};

struct kms_msg {
    uint32_t cmd;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
};

// KMS ring buffer at offset 128:
//   head(4B) + tail(4B) + kms_msg[240] (240 × 16B = 3840B) = 3848B
struct kms_ring {
    uint32_t head;                       // write position (0..239)
    uint32_t tail;                       // read position (0..239)
    struct kms_msg msgs[KMS_RING_SIZE];  // 240 slots × 16 bytes
};

#endif
