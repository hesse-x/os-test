#ifndef COMMON_SHM_H
#define COMMON_SHM_H

#include <stdint.h>

// Disk and FS shared memory are now dynamic (sys_shm_create/sys_shm_attach)
// KBD/KMS shared memory is also dynamic
// Internal offsets within each SHM region:

// Disk SHM: 5 pages, created by disk_driver
#define DISK_SHM_HEADER_OFFSET  0
#define DISK_REQ_OFFSET         4096    // 1 page in
#define DISK_RESP_OFFSET        12288   // 3 pages in

// FS SHM: 4 pages, created by fs_driver
#define FS_SHM_HEADER_OFFSET    0
#define FS_REQ_OFFSET           4096    // 1 page in
#define FS_RESP_OFFSET          8192    // 2 pages in

// Disk commands
#define DISK_CMD_READ   0
#define DISK_CMD_WRITE  1

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
    char     name[256]; // LFN long filename
    uint32_t size;      // file size
    uint32_t date;      // modification date (FAT32 wrt_date)
    uint32_t time;      // modification time (FAT32 wrt_time)
    uint8_t  attr;      // FAT attributes (dir/readonly etc)
    // 3 bytes padding → sizeof = 272
};

// FS RPC request (fits in recv_msg.data[56])
struct fs_rpc_request {
    uint32_t cmd;        // FS_CMD_*
    uint8_t  reserved[52];
};

// FS RPC reply (fits in 64 bytes, same as RECV_MSG_SIZE)
struct fs_rpc_reply {
    uint32_t status;     // 0=success, nonzero=error
    uint32_t fd;         // open returns fd
    uint32_t count;      // bytes read/returned
    uint32_t total;      // file size (open) or entry count (readdir)
    uint8_t  reserved[48];
};

// FS commands
#define FS_CMD_READDIR   0
#define FS_CMD_OPEN      1
#define FS_CMD_READ      2
#define FS_CMD_CLOSE     3
#define FS_CMD_RAW_READ  4
#define FS_CMD_CREATE    5   // touch: create empty file or update timestamp
#define FS_CMD_MKDIR     6   // mkdir: create directory

// Disk SHM header (sleeping flags for disk_driver <-> fs_driver)
struct disk_shm_header {
    uint8_t disk_driver_sleeping;   // disk_driver sets before sys_wait
    uint8_t fs_driver_sleeping;     // fs_driver sets before disk wait
    int32_t fs_driver_pid;          // fs_driver writes its PID after attach
    uint8_t reserved[2];
};

// FS SHM header (no sleeping flags — sync is via RPC now)
struct fs_shm_header {
    uint8_t reserved[8];
};

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

// ===================== Driver shared page layout =====================
// One 4K page created by kbd_driver via sys_shm_create(4096),
// attached by terminal via sys_shm_attach(sys_lookup_dev(DEV_KBD)).
//
// Offset 0:   driver_shm_header (8 bytes)
// Offset 8:   kbd ring buffer (head + tail + msgs[8] = 72 bytes, padded to 128)

#define KBD_RING_OFFSET  0

struct driver_shm_header {
    uint8_t kbd_sleeping;      // 1 = kbd_driver is sleeping
    uint8_t consumer_sleeping;  // 1 = consumer (terminal) is sleeping
    uint8_t reserved[6];
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

#endif
