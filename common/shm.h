#ifndef COMMON_SHM_H
#define COMMON_SHM_H

#include <stdint.h>

// Shared memory virtual addresses (fixed, mapped into all user processes)
// Layout: 10 pages (0x500000-0x509FFF)
//   0x500000  kbd_shm        (1 page)
//   0x501000  disk_req_shm   (2 pages, 0x501000-0x502FFF)
//   0x503000  disk_resp_shm  (2 pages, 0x503000-0x504FFF)
//   0x505000  fs_req_shm     (1 page)
//   0x506000  fs_resp_shm    (2 pages, 0x506000-0x507FFF)
//   0x508000  kms_info_shm   (1 page, kernel -> kms_driver)
//   0x509000  kms_req_shm    (1 page, client -> kms_driver)
#define KBD_SHM_ADDR    0x500000
#define DISK_REQ_ADDR   0x501000
#define DISK_REQ_ADDR2  0x502000   // second page of disk_req (expanded to 2 pages)
#define DISK_RESP_ADDR  0x503000
#define DISK_RESP_ADDR2 0x504000   // second page of disk_resp
#define FS_REQ_ADDR     0x505000
#define FS_RESP_ADDR    0x506000
#define FS_RESP_ADDR2   0x507000   // second page of fs_resp
#define KMS_INFO_ADDR   0x508000
#define KMS_REQ_ADDR    0x509000

// Keyboard shared page (driver -> consumer)
struct kbd_shm {
    uint32_t head;       // write position (ring buffer)
    uint32_t tail;       // read position
    uint8_t  data[4088]; // key event ring buffer
};

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

// KMS framebuffer info shared page (kernel -> kms_driver)
struct kms_fb_info {
    uint32_t width;      // pixel width
    uint32_t height;     // pixel height
    uint32_t pitch;      // bytes per line
    uint32_t bpp;        // bits per pixel
    uint64_t fb_vaddr;   // framebuffer virtual address in KMS process (0x700000)
    uint64_t fb_size;    // framebuffer size in bytes
    uint64_t fb_phys;    // framebuffer physical address (for KMS driver reference)
};

// KMS request commands
#define KMS_CMD_PUTC        0   // arg1=char, arg2=fg color
#define KMS_CMD_CLEAR       1   // no args
#define KMS_CMD_SCROLL      2   // no args, scroll up one line
#define KMS_CMD_CURSOR_MOVE 3   // arg1=x, arg2=y

struct kms_cmd {
    uint32_t cmd;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
};

// KMS request shared page (client -> kms_driver)
struct kms_req_shm {
    uint32_t count;              // number of commands
    uint32_t reserved;           // alignment
    struct kms_cmd cmds[255];    // command array: (4096-8)/16 = 255
};

#endif