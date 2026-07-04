#ifndef COMMON_SHM_H
#define COMMON_SHM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Disk and FS shared memory are now dynamic (memfd_create + ftruncate)
// KBD/KMS shared memory is also dynamic
// Internal offsets within each SHM region:

// FS SHM: 4 pages, created by fs_driver
#define FS_SHM_HEADER_OFFSET    0
#define FS_REQ_OFFSET           4096    // 1 page in
#define FS_RESP_OFFSET          8192    // 2 pages in

// FS request shared page (shell -> fs_driver)
typedef struct fs_req_shm {
    uint32_t cmd;        // FS_CMD_*
    uint32_t client_pid; // requester PID
    char     path[256];  // absolute path
    uint32_t fd;         // for read/close
    uint32_t offset;     // for read
    uint32_t count;      // for read
    uint32_t lba;        // for raw_read
} fs_req_shm_t;

// FS response shared page (fs_driver -> shell), 2 pages
typedef struct fs_resp_shm {
    uint32_t status;     // 0=success, nonzero=error
    uint32_t fd;         // open returns fd
    uint32_t count;      // bytes read/returned
    uint32_t total;      // file size (open) or entry count (readdir)
    uint8_t  data[8176]; // data area (2 pages - 16 bytes header)
} fs_resp_shm_t;

// Directory entry (readdir)
typedef struct fs_dirent {
    char     name[256]; // LFN long filename
    uint32_t size;      // file size
    uint32_t date;      // modification date (FAT32 wrt_date)
    uint32_t time;      // modification time (FAT32 wrt_time)
    uint8_t  attr;      // FAT attributes (dir/readonly etc)
    // 3 bytes padding → sizeof = 272
} fs_dirent_t;

// FS REQ request (fits in recv_msg.data[56])
typedef struct fs_req_request {
    uint32_t cmd;        // FS_CMD_*
    uint8_t  reserved[52];
} fs_req_request_t;

// FS REQ reply (fits in 64 bytes, same as RECV_MSG_SIZE)
typedef struct fs_req_reply {
    uint32_t status;     // 0=success, nonzero=error
    uint32_t fd;         // open returns fd
    uint32_t count;      // bytes read/returned
    uint32_t total;      // file size (open) or entry count (readdir)
    uint8_t  reserved[48];
} fs_req_reply_t;

// FS commands
#define FS_CMD_READDIR   0
#define FS_CMD_OPEN      1
#define FS_CMD_READ      2
#define FS_CMD_CLOSE     3
#define FS_CMD_RAW_READ  4
#define FS_CMD_CREATE    5   // touch: create empty file or update timestamp
#define FS_CMD_MKDIR     6   // mkdir: create directory

// FS SHM header (no sleeping flags — sync is via REQ now)
typedef struct fs_shm_header {
    uint8_t reserved[8];
} fs_shm_header_t;

// ===================== USB HID shared memory =====================
// 1 page (4KB) allocated by kernel xHCI init via shm_create_internal(1),
// registered as /dev/usb_hid via devtmpfs_create. kbd_driver opens it
// via open("/dev/usb_hid") + mmap(MAP_SHARED).
//
// Layout: 32B header + 4 sub-rings (keyboard/mouse/gamepad/touchpad)
// Each sub-ring: 100 slots × 10 bytes = 1000 bytes

#define USB_HID_SHM_MAGIC  0x55484944  // "UHID"
#define USB_HID_SHM_VERSION 1

#define HID_TYPE_KEYBOARD   1
#define HID_TYPE_MOUSE      2
#define HID_TYPE_GAMEPAD    3
#define HID_TYPE_TOUCHPAD   4

#define HID_SUBRING_KBD_OFFSET    32
#define HID_SUBRING_MOUSE_OFFSET  1032
#define HID_SUBRING_CAPACITY      100
#define HID_SLOT_SIZE             10

typedef struct usb_hid_slot {
    uint8_t  type;       // HID_TYPE_KEYBOARD etc.
    uint8_t  len;        // valid bytes in data[]
    uint8_t  data[8];    // raw HID report (keyboard: 8B Boot Protocol)
} usb_hid_slot_t;

typedef struct usb_hid_shm_header {
    uint32_t magic;      // USB_HID_SHM_MAGIC
    uint32_t version;    // USB_HID_SHM_VERSION
    struct {
        uint32_t head;
        uint32_t tail;
        uint32_t capacity;   // HID_SUBRING_CAPACITY
        uint32_t reserved;
    } rings[4];              // 4 sub-rings: kbd/mouse/gamepad/touchpad
} usb_hid_shm_header_t;

#ifdef __cplusplus
}
#endif

#endif
