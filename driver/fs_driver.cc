// ===================== FAT32 filesystem driver (user-space) — async event loop
// Receives requests via sys_msg (variable-length IPC), performs FAT32 operations,
// replies via sys_msg_resp. Accesses disk via disk_driver through
// DISK_REQ/DISK_RESP shared pages.
// Single-threaded event loop: disk I/O is async, client requests are
// processed via pending_op state machines with resume callbacks.
#include <stdint.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/device.h>
#include <sys/serial.h>
#include "common/shm.h"
#include "common/dev.h"
#include "common/errno.h"

// ===================== Shared page pointers (disk SHM only) =====================
static volatile disk_req_shm  *dreq;
static volatile disk_resp_shm *dresp;
static volatile disk_shm_header *disk_hdr;

// ===================== FAT32 volume state =====================
static uint32_t part_start_lba;
static uint32_t fat_start_lba;
static uint32_t data_start_lba;
static uint32_t root_cluster;
static uint32_t sectors_per_cluster;
static uint32_t bytes_per_cluster;
static uint32_t total_data_clusters;
static uint32_t spf32;

// ===================== Disk I/O helpers =====================
static int32_t disk_driver_pid;

// Synchronous disk I/O (only used during fat32_init)
static void disk_wait_reply_sync() {
    int32_t my_pid = getpid();
    while (1) {
        struct recv_msg m;
        recv(&m, NULL, 0, 0);
        if (m.type == RECV_NOTIFY && (int32_t)m.src == disk_driver_pid) {
            return;
        }
        notify(my_pid);
    }
}

static int disk_read_sync(uint32_t lba, uint32_t count) {
    dreq->cmd   = DISK_CMD_READ;
    dreq->lba   = lba;
    dreq->count = count;
    disk_hdr->fs_driver_sleeping = 1;
    if (disk_hdr->disk_driver_sleeping) {
        notify(disk_driver_pid);
    }
    disk_wait_reply_sync();
    disk_hdr->fs_driver_sleeping = 0;
    return dresp->status;
}

static int disk_write_sync(uint32_t lba, uint32_t count, const uint8_t *data) {
    dreq->cmd   = DISK_CMD_WRITE;
    dreq->lba   = lba;
    dreq->count = count;
    uint32_t bytes = count * 512;
    __memcpy((void *)dreq->data, data, bytes);
    disk_hdr->fs_driver_sleeping = 1;
    if (disk_hdr->disk_driver_sleeping) {
        notify(disk_driver_pid);
    }
    disk_wait_reply_sync();
    disk_hdr->fs_driver_sleeping = 0;
    return dresp->status;
}

// ===================== disk_io: async disk I/O descriptor =====================
struct disk_io;

struct disk_io {
    uint32_t lba;
    uint32_t count;
    void *dest;                     // read: memcpy target; write: NULL
    void (*complete)(disk_io *io);  // completion callback
    void *ctx;                      // callback context (typically pending_op*)
    disk_io *next;                  // queue link
};

#define DISK_IO_POOL_SIZE 32
static disk_io disk_io_pool[DISK_IO_POOL_SIZE];
static bool disk_io_used[DISK_IO_POOL_SIZE];

static disk_io *disk_io_alloc() {
    for (int i = 0; i < DISK_IO_POOL_SIZE; i++) {
        if (!disk_io_used[i]) {
            disk_io_used[i] = true;
            return &disk_io_pool[i];
        }
    }
    return NULL;
}

static void disk_io_free(disk_io *io) {
    int idx = io - disk_io_pool;
    if (idx >= 0 && idx < DISK_IO_POOL_SIZE) disk_io_used[idx] = false;
}

// Disk FIFO queue
static disk_io *disk_queue_head = NULL;
static disk_io *disk_queue_tail = NULL;
static bool disk_in_flight = false;
static disk_io *current_disk_io = NULL;

static void submit_disk_io(disk_io *io);

static void submit_next_disk_io() {
    if (disk_queue_head == NULL) return;
    disk_io *io = disk_queue_head;
    disk_queue_head = io->next;
    if (disk_queue_head == NULL) disk_queue_tail = NULL;
    io->next = NULL;
    submit_disk_io(io);
}

static void submit_disk_io(disk_io *io) {
    if (disk_in_flight) {
        // Enqueue at tail
        io->next = NULL;
        if (disk_queue_tail) disk_queue_tail->next = io;
        else disk_queue_head = io;
        disk_queue_tail = io;
        return;
    }

    disk_in_flight = true;
    current_disk_io = io;

    // Determine read vs write by dest field (NULL = write, non-NULL = read)
    if (io->dest != NULL) {
        dreq->cmd = DISK_CMD_READ;
    } else {
        dreq->cmd = DISK_CMD_WRITE;
    }
    dreq->lba = io->lba;
    dreq->count = io->count;

    disk_hdr->fs_driver_sleeping = 1;
    if (disk_hdr->disk_driver_sleeping) {
        notify(disk_driver_pid);
    }
}

// Called when disk_driver sends completion notification
static void handle_disk_complete() {
    disk_io *io = current_disk_io;
    if (!io) {
        serial_write("fs: disk_complete but no io!\n", 29);
        return;
    }

    // For reads: memcpy from dresp to io->dest before callback
    if (io->dest != NULL) {
        if (dresp->status != 0) {
            serial_write("fs: disk read error!\n", 22);
        }
        __memcpy(io->dest, (const void *)dresp->data, io->count * 512);
    }

    disk_hdr->fs_driver_sleeping = 0;
    disk_in_flight = false;
    current_disk_io = NULL;

    io->complete(io);

    submit_next_disk_io();
}

// Helper: submit a disk read that fills a cache slot
static void submit_disk_read(uint32_t lba, uint32_t count, void *dest,
                              void (*complete)(disk_io *), void *ctx) {
    disk_io *io = disk_io_alloc();
    if (!io) return; // pool exhausted
    io->lba = lba;
    io->count = count;
    io->dest = dest;
    io->complete = complete;
    io->ctx = ctx;
    io->next = NULL;
    submit_disk_io(io);
}

// Helper: submit a disk write (data already in dreq->data via caller)
static void submit_disk_write(uint32_t lba, uint32_t count,
                               void (*complete)(disk_io *), void *ctx) {
    disk_io *io = disk_io_alloc();
    if (!io) return;
    io->lba = lba;
    io->count = count;
    io->dest = NULL; // NULL signals write
    io->complete = complete;
    io->ctx = ctx;
    io->next = NULL;
    submit_disk_io(io);
}

// ===================== Data cluster LRU cache (16 slots) =====================
#define CACHE_SLOTS 16

struct cache_entry {
    uint32_t cluster;   // 0xFFFFFFFF = invalid
    uint8_t  data[4096];
    uint32_t age;
    uint32_t pin_count; // >0 prevents eviction
};

static cache_entry cache[CACHE_SLOTS];
static uint32_t cache_time = 0;

static int cache_lookup(uint32_t cluster) {
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].cluster == cluster) {
            cache[i].age = ++cache_time;
            return i;
        }
    }
    return -1;
}

static int cache_alloc(uint32_t cluster) {
    int best = -1;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].pin_count > 0) continue;
        if (best < 0 || cache[i].age < cache[best].age) best = i;
    }
    if (best < 0) return -1; // all pinned
    cache[best].cluster = cluster;
    cache[best].age = ++cache_time;
    return best;
}

static void cache_invalidate(uint32_t cluster) {
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].cluster == cluster) {
            cache[i].cluster = 0xFFFFFFFF;
            cache[i].age = 0;
        }
    }
}

static void cache_pin(uint32_t cluster) {
    int slot = cache_lookup(cluster);
    if (slot >= 0) cache[slot].pin_count++;
}

static void cache_unpin(uint32_t cluster) {
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].cluster == cluster && cache[i].pin_count > 0) {
            cache[i].pin_count--;
        }
    }
}

// ===================== FAT sector cache (4 pages, fixed) =====================
#define FAT_CACHE_PAGES 4

struct fat_cache_entry {
    uint32_t sector_lba;
    uint8_t  data[512];
};

static fat_cache_entry fat_cache[FAT_CACHE_PAGES];
static uint32_t fat_cache_time = 0;
static uint32_t fat_cache_age[FAT_CACHE_PAGES];

static int fat_cache_lookup(uint32_t sector_lba) {
    for (int i = 0; i < FAT_CACHE_PAGES; i++) {
        if (fat_cache[i].sector_lba == sector_lba) {
            fat_cache_age[i] = ++fat_cache_time;
            return i;
        }
    }
    return -1;
}

static int fat_cache_alloc(uint32_t sector_lba) {
    int best = 0;
    for (int i = 1; i < FAT_CACHE_PAGES; i++) {
        if (fat_cache_age[i] < fat_cache_age[best]) best = i;
    }
    fat_cache[best].sector_lba = sector_lba;
    fat_cache_age[best] = ++fat_cache_time;
    return best;
}

// Sync FAT cache read (for init only)
static int fat_cache_read_sync(uint32_t sector_lba) {
    int slot = fat_cache_lookup(sector_lba);
    if (slot >= 0) return slot;

    if (disk_read_sync(sector_lba, 1) != 0) return -1;

    slot = fat_cache_alloc(sector_lba);
    __memcpy(fat_cache[slot].data, (const void *)dresp->data, 512);
    return slot;
}

// Sync cluster read (for init only)
static int read_cluster_sync(uint32_t cluster) {
    int slot = cache_lookup(cluster);
    if (slot >= 0) return slot;

    slot = cache_alloc(cluster);
    uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;

    if (disk_read_sync(lba, sectors_per_cluster) != 0) {
        cache[slot].cluster = 0xFFFFFFFF;
        return -1;
    }

    __memcpy(cache[slot].data, (const void *)dresp->data, bytes_per_cluster);
    return slot;
}

// ===================== FAT chain traversal (sync, for init) =====================
static uint32_t fat_read_entry_sync(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;

    int slot = fat_cache_read_sync(fat_sector);
    if (slot < 0) return 0x0FFFFFFF;

    uint8_t *src = fat_cache[slot].data + offset_in_sector;
    uint32_t entry_val = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                         ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
    return entry_val & 0x0FFFFFFF;
}

// ===================== FAT directory entry (32 bytes) =====================
struct fat_dir_entry {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} __attribute__((packed));

// ===================== 8.3 name helpers =====================

static void format_83_name(const char *user, int user_len, char out[11]) {
    if (user_len == 1 && user[0] == '.') {
        out[0] = '.';
        for (int i = 1; i < 11; i++) out[i] = ' ';
        return;
    }
    if (user_len == 2 && user[0] == '.' && user[1] == '.') {
        out[0] = '.';
        out[1] = '.';
        for (int i = 2; i < 11; i++) out[i] = ' ';
        return;
    }

    for (int i = 0; i < 11; i++) out[i] = ' ';

    int i = 0, j = 0;
    while (i < user_len && user[i] != '.' && j < 8) {
        char c = user[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = c;
        i++;
    }
    if (i < user_len && user[i] == '.') {
        i++;
        j = 8;
        while (i < user_len && j < 11) {
            char c = user[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[j++] = c;
            i++;
        }
    }
}

static void format_83_name_nt(const char *user, char out[11]) {
    int len = 0;
    while (user[len]) len++;
    format_83_name(user, len, out);
}

static void convert_83_to_name(const char stored[11], uint8_t nt_res, char *out, int out_len) {
    int j = 0;
    for (int i = 0; i < 8 && j < out_len - 1; i++) {
        char c = stored[i];
        if (c != ' ') {
            if ((nt_res & 0x08) && c >= 'A' && c <= 'Z') c += 32;
            out[j++] = c;
        }
    }
    bool has_ext = false;
    for (int i = 8; i < 11; i++) {
        if (stored[i] != ' ') { has_ext = true; break; }
    }
    if (has_ext && j < out_len - 1) {
        out[j++] = '.';
        for (int i = 8; i < 11 && j < out_len - 1; i++) {
            char c = stored[i];
            if (c != ' ') {
                if ((nt_res & 0x10) && c >= 'A' && c <= 'Z') c += 32;
                out[j++] = c;
            }
        }
    }
    out[j] = '\0';
}

static bool match_83_name(const char stored[11], const char *name, int name_len) {
    char expanded[11];
    format_83_name(name, name_len, expanded);
    for (int i = 0; i < 11; i++) {
        if (stored[i] != expanded[i]) return false;
    }
    return true;
}

// ===================== LFN helpers =====================

static bool collect_lfn_entry(const fat_dir_entry *de, char *lfn_buf) {
    const uint8_t *raw = (const uint8_t *)de;
    int seq = raw[0] & 0x3F;

    static const int offsets[] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    static const int n_chars = 13;

    int base = (seq - 1) * n_chars;

    for (int c = 0; c < n_chars; c++) {
        uint8_t lo = raw[offsets[c]];
        uint8_t hi = raw[offsets[c] + 1];

        if (hi != 0) {
            lfn_buf[0] = '\0';
            return false;
        }

        if (lo == 0x00 || lo == 0xFF) {
            lfn_buf[base + c] = '\0';
            for (int k = base + c + 1; k < 256; k++) lfn_buf[k] = '\0';
            return true;
        }

        lfn_buf[base + c] = (char)lo;
    }
    return true;
}

static bool match_lfn_name(const char *lfn_buf, const char *name, int name_len) {
    for (int i = 0; i < name_len; i++) {
        char lc = lfn_buf[i];
        char nc = name[i];
        if (lc == '\0') return false;
        if (lc >= 'a' && lc <= 'z') lc -= 32;
        if (nc >= 'a' && nc <= 'z') nc -= 32;
        if (lc != nc) return false;
    }
    return lfn_buf[name_len] == '\0';
}

static uint8_t lfn_checksum(const uint8_t *name) {
    uint8_t sum = 0;
    for (int i = 11; i; i--)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *name++;
    return sum;
}

// ---- dir_slot for create/mkdir ----
struct dir_slot {
    uint32_t cluster;
    int index;
};

// ---- Timestamp ----
#define HARD_DATE 0x5A21
#define HARD_TIME 0x0000

// ===================== Message protocol =====================
#define FILE_CMD_OPEN      1
#define FILE_CMD_READ      2
#define FILE_CMD_WRITE     3
#define FILE_CMD_CLOSE     4
#define FILE_CMD_READDIR   5
#define FILE_CMD_CREATE    6
#define FILE_CMD_MKDIR     7
#define FILE_CMD_RAW_READ  8
#define FILE_CMD_STAT      9
#define FILE_CMD_PING     10

struct file_req {
    uint32_t cmd;
    char     path[256];
    uint32_t flags;
    uint32_t fs_fd;
    uint64_t offset;
    uint32_t count;
    uint32_t lba;
    uint32_t readdir_offset;
    uint32_t readdir_count;
};

struct file_resp {
    int32_t  status;
    uint32_t fd;
    uint64_t file_size;
    uint32_t count;
    uint32_t total;
    uint8_t  data[];
};

#define MAX_REPLY_DATA  (65536)

// ===================== pending_op: async request state =====================

enum op_type {
    OP_READ,
    OP_OPEN,
    OP_READDIR,
    OP_RAW_READ,
    OP_CREATE,
    OP_MKDIR,
    OP_STAT
};

struct pending_op;
struct pending_op {
    int client_pid;
    int session_idx;        // index into sessions[]
    op_type type;
    void (*resume)(pending_op *op);
    disk_io io;             // embedded disk_io for this op
    bool io_active;         // true if io is submitted

    // Per-operation context
    union {
        // READ
        struct {
            uint32_t fs_fd;
            uint64_t offset;
            uint32_t count;
            uint32_t current_cluster;
            uint32_t chain_pos;
            uint32_t offset_clusters;
            uint32_t in_cluster_offset;
            uint32_t bytes_read;
            uint32_t ra_cluster;
            uint32_t ra_count;
            bool     ra_sequential;
            bool     ra_detected;
            uint32_t fat_sector_lba;    // for async FAT chain walk
        } read;

        // OPEN
        struct {
            char path[256];
            uint32_t flags;
            uint32_t dir_cluster;
            uint32_t current_cluster;
            int comp_start;         // path component start index
            int comp_len;           // path component length
            bool path_done;         // finished path resolution
            fat_dir_entry de;
            uint32_t de_cluster;
            // find_dir_entry state
            int entry_idx;
            char lfn_buf[256];
        } open;

        // READDIR
        struct {
            char path[256];
            uint32_t rd_offset;
            uint32_t rd_count;
            uint32_t dir_cluster;
            uint32_t current_cluster;
            char lfn_buf[256];
            uint32_t entry_count;
            uint32_t out_count;
            int entry_idx;
        } readdir;

        // RAW_READ
        struct {
            uint32_t lba;
            uint32_t count;
        } raw_read;

        // CREATE
        struct {
            char path[256];
            int phase;             // 0=resolve_parent, 1=check_existing, 2=gen_short, 3=find_slots, 4=write_lfn, 5=write_entry
            uint32_t parent_cluster;
            char leaf_name[256];
            int leaf_len;
            char short_name[11];
            int lfn_count;
            int total_slots;
            dir_slot slots[20];
            fat_dir_entry de;
            uint32_t de_cluster;
            uint32_t current_cluster;
            int entry_idx;
            char lfn_buf[256];
            uint8_t sector_buf[512];
        } create;

        // MKDIR
        struct {
            char path[256];
            int phase;
            uint32_t parent_cluster;
            char leaf_name[256];
            int leaf_len;
            char short_name[11];
            uint32_t new_dir_cluster;
            int lfn_count;
            int total_slots;
            dir_slot slots[20];
            uint8_t cluster_data_buf[4096];
            uint8_t sector_buf[512];
            uint32_t write_sector_idx;
        } mkdir;

        // STAT
        struct {
            char path[256];
            uint32_t dir_cluster;
            uint32_t current_cluster;
            int comp_start;
            int comp_len;
            bool path_done;
            fat_dir_entry de;
            uint32_t de_cluster;
            int entry_idx;
            char lfn_buf[256];
        } stat;
    } u;

    // Per-op reply buffer
    uint8_t reply_buf[sizeof(file_resp) + 65536];
};

#define MAX_PENDING_OPS 16
static pending_op pending_pool[MAX_PENDING_OPS];

static pending_op *alloc_pending_op() {
    for (int i = 0; i < MAX_PENDING_OPS; i++) {
        if (pending_pool[i].client_pid == -1) {
            pending_pool[i].client_pid = -2; // mark as in-use
            return &pending_pool[i];
        }
    }
    return NULL;
}

static void free_pending_op(pending_op *op) {
    op->client_pid = -1;
}

    // Helper: send reply and free pending_op
static void op_complete(pending_op *op, int32_t status, uint32_t count) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = status;
    resp->count = count;
    serial_write("fs: op_complete\n", 17);
    msg_resp(op->reply_buf, sizeof(file_resp) + count);
    free_pending_op(op);
}

// ===================== Async FAT helpers =====================

// Read FAT entry from cache only (no disk I/O). Returns 0xFFFFFFFF on miss.
static uint32_t fat_read_entry_cached(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;

    int slot = fat_cache_lookup(fat_sector);
    if (slot < 0) return 0xFFFFFFFF; // miss

    uint8_t *src = fat_cache[slot].data + offset_in_sector;
    uint32_t entry_val = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                         ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
    return entry_val & 0x0FFFFFFF;
}

// ===================== Async read_cluster / fat_cache_read =====================
// These functions check cache; on hit return slot index, on miss submit disk_io
// and return -1. The caller's resume function will be called on completion.

static void read_cluster_resume(disk_io *io);
static void fat_cache_read_resume(disk_io *io);

// Returns cache slot on hit, -1 on miss (disk_io submitted)
static int read_cluster_async(uint32_t cluster, pending_op *op) {
    int slot = cache_lookup(cluster);
    if (slot >= 0) return slot;

    slot = cache_alloc(cluster);
    if (slot < 0) return -1; // all pinned

    uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;
    op->io.lba = lba;
    op->io.count = sectors_per_cluster;
    op->io.dest = cache[slot].data;
    op->io.complete = read_cluster_resume;
    op->io.ctx = op;
    op->io.next = NULL;
    op->io_active = true;
    submit_disk_io(&op->io);
    return -1;
}

static void read_cluster_resume(disk_io *io) {
    pending_op *op = (pending_op *)io->ctx;
    op->io_active = false;
    op->resume(op);
}

// Returns FAT cache slot on hit, -1 on miss
static int fat_cache_read_async(uint32_t sector_lba, pending_op *op) {
    int slot = fat_cache_lookup(sector_lba);
    if (slot >= 0) return slot;

    slot = fat_cache_alloc(sector_lba);

    op->io.lba = sector_lba;
    op->io.count = 1;
    op->io.dest = fat_cache[slot].data;
    op->io.complete = fat_cache_read_resume;
    op->io.ctx = op;
    op->io.next = NULL;
    op->io_active = true;
    submit_disk_io(&op->io);
    return -1;
}

static void fat_cache_read_resume(disk_io *io) {
    pending_op *op = (pending_op *)io->ctx;
    op->io_active = false;
    op->resume(op);
}

// ===================== Async FAT write =====================
// fat_write_entry needs sector_buf. The caller must provide one.
// For simplicity, we do FAT writes synchronously from within the event loop
// by submitting disk_write and waiting via resume. The write path:
// 1. Read FAT sector (async, via cache)
// 2. Modify and write back FAT1 (disk_write)
// 3. Write FAT2 (disk_write)
// This is complex to fully async, so we use a multi-phase approach.

// Async fat_write_entry: phases 0=read FAT sector, 1=write FAT1, 2=write FAT2
// State stored in a small struct embedded in create/mkdir context.

struct fat_write_state {
    uint32_t cluster;
    uint32_t value;
    uint32_t fat_sector;
    uint32_t fat2_sector;
    uint32_t offset_in_sector;
    int fat_cache_slot;
    int phase; // 0=need FAT read, 1=FAT read done write FAT1, 2=FAT1 done write FAT2, 3=done
    uint8_t sector_buf[512];
};

// ===================== Async write_dir_entry_at =====================
// write_dir_entry_at: read cluster (async), modify, write sector(s) (async)
// Phase 0=read cluster, 1=write sector, 2=write second sector (if cross-boundary), 3=done

struct write_dir_state {
    uint32_t cluster;
    int index;
    fat_dir_entry entry;
    int cache_slot;
    int phase; // 0=need read, 1=write sector 1, 2=write sector 2, 3=done
    int sector_in_cluster;
    int need_second_sector;
    int result;
};

// ===================== Multi-client session management =====================
#define MAX_CLIENTS  16
#define MAX_SESSION_FDS 8

struct session_open_file {
    bool     used;
    uint32_t start_cluster;
    uint32_t file_size;
    uint64_t ra_prev_offset;
    uint32_t ra_prev_count;
    bool     ra_sequential;
};

struct client_session {
    pid_t client_pid;
    struct session_open_file open_files[MAX_SESSION_FDS];
};

static struct client_session sessions[MAX_CLIENTS];

static struct client_session *get_session(pid_t pid) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].client_pid == pid) return &sessions[i];
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].client_pid == -1) {
            sessions[i].client_pid = pid;
            for (int j = 0; j < MAX_SESSION_FDS; j++)
                sessions[i].open_files[j].used = false;
            return &sessions[i];
        }
    }
    return NULL;
}

static int session_alloc_fd(struct client_session *sess) {
    for (int i = 0; i < MAX_SESSION_FDS; i++) {
        if (!sess->open_files[i].used) return i;
    }
    return -1;
}

// ===================== write_lock =====================
static bool write_lock_held = false;
static pending_op *write_waiter_head = NULL;
static pending_op *write_waiter_tail = NULL;

static bool acquire_write_lock(pending_op *op) {
    if (!write_lock_held) {
        write_lock_held = true;
        return true;
    }
    // Enqueue
    op->io.next = NULL; // reuse next as waiter link (safe: io not active during wait)
    if (write_waiter_tail) {
        // Use a simple linked list via a separate field
        // We'll store waiter link in the pending_op itself
        // using a small array for waiter queue
    }
    return false; // would block
}

// Simple waiter queue using array
#define WRITE_WAIT_QUEUE_SIZE 8
static pending_op *write_wait_queue[WRITE_WAIT_QUEUE_SIZE];
static int write_wait_head = 0;
static int write_wait_tail = 0;

static bool write_lock_acquire(pending_op *op) {
    if (!write_lock_held) {
        write_lock_held = true;
        return true;
    }
    int next = (write_wait_tail + 1) % WRITE_WAIT_QUEUE_SIZE;
    if (next == write_wait_head) return false; // queue full
    write_wait_queue[write_wait_tail] = op;
    write_wait_tail = next;
    return false; // queued
}

static void write_lock_release() {
    if (write_wait_head != write_wait_tail) {
        pending_op *op = write_wait_queue[write_wait_head];
        write_wait_head = (write_wait_head + 1) % WRITE_WAIT_QUEUE_SIZE;
        op->resume(op); // resume the waiter
    } else {
        write_lock_held = false;
    }
}

// ===================== Readahead (async) =====================
static bool readahead_pending = false;
static disk_io readahead_io;

static void readahead_complete(disk_io *io);

// Submit async readahead: prefetch clusters into cache
static void submit_readahead(uint32_t start_cluster, uint32_t num_clusters) {
    if (readahead_pending) return;
    if (start_cluster < 2 || start_cluster >= 0x0FFFFFF8) return;

    readahead_pending = true;

    // Walk FAT chain via cache, find consecutive clusters to prefetch
    uint32_t c = start_cluster;
    uint32_t consecutive_start = c;
    uint32_t consecutive_count = 0;
    uint32_t remaining = num_clusters;
    if (remaining > 4) remaining = 4;

    // Check if already cached
    if (cache_lookup(c) >= 0) {
        readahead_pending = false;
        return;
    }

    // Simple: prefetch one batch of consecutive clusters
    // Find how many consecutive clusters from start
    while (remaining > 0 && c >= 2 && c < 0x0FFFFFF8) {
        uint32_t next = fat_read_entry_cached(c);
        if (next == 0xFFFFFFFF) break; // FAT miss, skip
        if (next != c + 1 && consecutive_count > 0) break;
        consecutive_count++;
        remaining--;
        c = next;
    }

    if (consecutive_count == 0) {
        // Single cluster prefetch
        consecutive_start = start_cluster;
        consecutive_count = 1;
    }

    // Find cache slot for first cluster
    int slot = cache_alloc(consecutive_start);
    if (slot < 0) { readahead_pending = false; return; }

    uint32_t lba = data_start_lba + (consecutive_start - 2) * sectors_per_cluster;
    uint32_t total_sectors = consecutive_count * sectors_per_cluster;
    if (total_sectors > 32) { total_sectors = 32; consecutive_count = total_sectors / sectors_per_cluster; }

    // For simplicity, prefetch just the first cluster's worth
    // (multi-cluster readahead requires distributing data to multiple cache slots)
    readahead_io.lba = lba;
    readahead_io.count = sectors_per_cluster;
    readahead_io.dest = cache[slot].data;
    readahead_io.complete = readahead_complete;
    readahead_io.ctx = NULL;
    readahead_io.next = NULL;
    submit_disk_io(&readahead_io);
}

static void readahead_complete(disk_io *io) {
    readahead_pending = false;
    // Data already copied to cache slot by handle_disk_complete
}

// ===================== Async handlers =====================

// Forward declarations
static void resume_read(pending_op *op);
static void resume_open(pending_op *op);
static void resume_readdir(pending_op *op);
static void resume_raw_read(pending_op *op);
static void resume_create(pending_op *op);
static void resume_mkdir(pending_op *op);
static void resume_stat(pending_op *op);

// ---- READ ----
static void start_read(pending_op *op, struct client_session *sess,
                        uint32_t fs_fd, uint64_t offset, uint32_t count) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = 0;
    resp->fd = 0;
    resp->file_size = 0;
    resp->count = 0;
    resp->total = 0;

    if (fs_fd >= MAX_SESSION_FDS || !sess->open_files[fs_fd].used) {
        op_complete(op, -EBADF, 0);
        return;
    }

    session_open_file *f = &sess->open_files[fs_fd];
    if (offset >= f->file_size) {
        op_complete(op, 0, 0);
        return;
    }

    // Sequential detection
    if (f->ra_prev_count > 0 && offset == f->ra_prev_offset + f->ra_prev_count) {
        f->ra_sequential = true;
        op->u.read.ra_detected = true;
    } else if (offset != 0) {
        f->ra_sequential = false;
        op->u.read.ra_detected = false;
    } else {
        op->u.read.ra_detected = false;
    }
    op->u.read.ra_sequential = f->ra_sequential;
    f->ra_prev_offset = offset;
    f->ra_prev_count = count;

    uint32_t avail = f->file_size - (uint32_t)offset;
    if (count > avail) count = avail;
    if (count > MAX_REPLY_DATA) count = MAX_REPLY_DATA;

    op->u.read.fs_fd = fs_fd;
    op->u.read.offset = offset;
    op->u.read.count = count;
    op->u.read.current_cluster = f->start_cluster;
    op->u.read.chain_pos = 0;
    op->u.read.offset_clusters = (uint32_t)offset / bytes_per_cluster;
    op->u.read.in_cluster_offset = (uint32_t)offset % bytes_per_cluster;
    op->u.read.bytes_read = 0;
    op->u.read.ra_cluster = 0;
    op->u.read.ra_count = 0;

    op->resume = resume_read;
    resume_read(op);
}

static void resume_read(pending_op *op) {
    file_resp *resp = (file_resp *)op->reply_buf;

    // Phase 1: FAT chain walk to reach offset cluster
    while (op->u.read.chain_pos < op->u.read.offset_clusters) {
        // Try to read FAT entry from cache
        uint32_t next = fat_read_entry_cached(op->u.read.current_cluster);
        if (next == 0xFFFFFFFF) {
            // FAT cache miss — need to read FAT sector
            uint32_t fat_offset = op->u.read.current_cluster * 4;
            uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
            int slot = fat_cache_read_async(fat_sector, op);
            if (slot < 0) return; // disk_io submitted, will call resume_read
            // FAT sector now in cache, try again
            continue;
        }
        if (next >= 0x0FFFFFF8) {
            op_complete(op, -EINVAL, 0);
            return;
        }
        op->u.read.current_cluster = next;
        op->u.read.chain_pos++;
    }

    // Phase 2: Read data clusters
    while (op->u.read.bytes_read < op->u.read.count &&
           op->u.read.current_cluster >= 2 && op->u.read.current_cluster < 0x0FFFFFF8) {
        int slot = read_cluster_async(op->u.read.current_cluster, op);
        if (slot < 0) return; // disk_io submitted

        uint32_t to_copy = bytes_per_cluster - op->u.read.in_cluster_offset;
        if (to_copy > op->u.read.count - op->u.read.bytes_read)
            to_copy = op->u.read.count - op->u.read.bytes_read;

        __memcpy(resp->data + op->u.read.bytes_read,
                 cache[slot].data + op->u.read.in_cluster_offset, to_copy);
        op->u.read.bytes_read += to_copy;
        op->u.read.in_cluster_offset = 0;

        // Advance to next cluster via FAT cache
        uint32_t next = fat_read_entry_cached(op->u.read.current_cluster);
        if (next == 0xFFFFFFFF) {
            // Rare: FAT miss during data phase
            uint32_t fat_offset = op->u.read.current_cluster * 4;
            uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
            if (fat_cache_read_async(fat_sector, op) < 0) return;
            continue;
        }
        op->u.read.current_cluster = next;
    }

    // Done: set readahead info
    if (op->u.read.ra_sequential &&
        op->u.read.current_cluster >= 2 && op->u.read.current_cluster < 0x0FFFFFF8) {
        op->u.read.ra_cluster = op->u.read.current_cluster;
        op->u.read.ra_count = 4;
    }

    resp->status = 0;
    resp->count = op->u.read.bytes_read;
    size_t resp_len = sizeof(file_resp) + resp->count;
    msg_resp(op->reply_buf, resp_len);

    // Async readahead
    if (op->u.read.ra_cluster > 0) {
        submit_readahead(op->u.read.ra_cluster, op->u.read.ra_count);
    }

    free_pending_op(op);
}

// ---- OPEN ----
static void start_open(pending_op *op, struct client_session *sess,
                        const char *path, uint32_t flags) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = 0; resp->fd = 0; resp->file_size = 0; resp->count = 0; resp->total = 0;

    if (path[0] != '/') {
        op_complete(op, -ENOENT, 0);
        return;
    }

    for (int i = 0; i < 256 && path[i]; i++) op->u.open.path[i] = path[i];

    op->u.open.flags = flags;
    op->u.open.dir_cluster = root_cluster;
    op->u.open.current_cluster = root_cluster;
    op->u.open.comp_start = 1; // skip leading '/'
    op->u.open.comp_len = 0;
    op->u.open.path_done = false;
    op->u.open.lfn_buf[0] = '\0';
    op->u.open.entry_idx = 0;

    // Check if path is just "/"
    if (path[1] == '\0') {
        // Root directory
        if (op->type == OP_OPEN) {
            op_complete(op, -EISDIR, 0);
            return;
        }
        // For stat
        resp->status = 0;
        resp->file_size = 0;
        op_complete(op, 0, 0);
        return;
    }

    // Parse first path component
    const char *p = path + 1;
    op->u.open.comp_start = 1;
    op->u.open.comp_len = 0;
    while (p[op->u.open.comp_len] && p[op->u.open.comp_len] != '/')
        op->u.open.comp_len++;

    op->u.open.current_cluster = root_cluster;
    op->resume = resume_open;
    resume_open(op);
}

static void resume_open(pending_op *op) {
    file_resp *resp = (file_resp *)op->reply_buf;
    char *path = op->u.open.path;
    struct client_session *sess = &sessions[op->session_idx];

    // Debug: show current state
    {
        char dbg[80];
        int p = 0;
        for (const char *s = "fs: resume_open cs="; *s; s++) dbg[p++] = *s;
        // comp_start as decimal
        int cs = op->u.open.comp_start;
        if (cs < 0) { dbg[p++] = '-'; cs = -cs; }
        char num[12]; int nl = 0;
        if (cs == 0) num[nl++] = '0';
        else { int t = cs; while (t) { num[nl++] = '0' + t % 10; t /= 10; } }
        for (int i = nl - 1; i >= 0; i--) dbg[p++] = num[i];
        dbg[p++] = ' '; dbg[p++] = 'c'; dbg[p++] = 'l';
        dbg[p++] = '=';
        int cl = op->u.open.comp_len;
        nl = 0; if (cl == 0) num[nl++] = '0';
        else { int t = cl; while (t) { num[nl++] = '0' + t % 10; t /= 10; } }
        for (int i = nl - 1; i >= 0; i--) dbg[p++] = num[i];
        dbg[p++] = ' '; dbg[p++] = 'e'; dbg[p++] = 'i';
        dbg[p++] = '=';
        int ei = op->u.open.entry_idx;
        nl = 0; if (ei == 0) num[nl++] = '0';
        else { int t = ei; while (t) { num[nl++] = '0' + t % 10; t /= 10; } }
        for (int i = nl - 1; i >= 0; i--) dbg[p++] = num[i];
        dbg[p++] = ' '; dbg[p++] = 'c'; dbg[p++] = 'c';
        dbg[p++] = '=';
        uint32_t cc = op->u.open.current_cluster;
        nl = 0; if (cc == 0) num[nl++] = '0';
        else { uint32_t t = cc; while (t) { num[nl++] = '0' + t % 10; t /= 10; } }
        for (int i = nl - 1; i >= 0; i--) dbg[p++] = num[i];
        dbg[p++] = '\n';
        serial_write(dbg, p);
    }

    // Find next path component
    while (1) {
open_completion_check:
        // Check if we've processed all components
        int comp_start = op->u.open.comp_start;
        int comp_len = op->u.open.comp_len;

        if (comp_len == 0 || path[comp_start] == '\0') {
            // Path resolution complete — matched entry found earlier
            if (op->type == OP_OPEN) {
                if (op->u.open.de.attr & 0x10) {
                    op_complete(op, -EISDIR, 0);
                    return;
                }
                int fd = session_alloc_fd(sess);
                if (fd < 0) { op_complete(op, -EMFILE, 0); return; }
                uint32_t cluster = ((uint32_t)op->u.open.de.fst_clus_hi << 16) | op->u.open.de.fst_clus_lo;
                if (cluster == 0) cluster = root_cluster;
                sess->open_files[fd].used = true;
                sess->open_files[fd].start_cluster = cluster;
                sess->open_files[fd].file_size = op->u.open.de.file_size;
                sess->open_files[fd].ra_prev_offset = 0;
                sess->open_files[fd].ra_prev_count = 0;
                sess->open_files[fd].ra_sequential = false;
                resp->status = 0;
                resp->fd = (uint32_t)fd;
                resp->file_size = op->u.open.de.file_size;
                op_complete(op, 0, 0);
            } else {
                // STAT
                resp->status = 0;
                resp->file_size = op->u.open.de.file_size;
                op_complete(op, 0, 0);
            }
            return;
        }

        // Need to find component in current directory (current_cluster)
        // find_dir_entry loop
        while (op->u.open.current_cluster >= 2 && op->u.open.current_cluster < 0x0FFFFFF8) {
            // Re-read locals after descent (comp_start/comp_len may have changed)
            comp_start = op->u.open.comp_start;
            comp_len = op->u.open.comp_len;
            int slot = read_cluster_async(op->u.open.current_cluster, op);
            if (slot < 0) return; // disk I/O pending

            uint8_t *data = cache[slot].data;
            int entries = bytes_per_cluster / 32;
            for (; op->u.open.entry_idx < entries; op->u.open.entry_idx++) {
                fat_dir_entry *de = (fat_dir_entry *)(data + op->u.open.entry_idx * 32);
                if (de->name[0] == 0x00) {
                    // End of dir — not found
                    op_complete(op, -ENOENT, 0);
                    return;
                }
                if (de->name[0] == 0xE5) { op->u.open.lfn_buf[0] = '\0'; continue; }
                if (de->attr == 0x0F) {
                    collect_lfn_entry(de, op->u.open.lfn_buf);
                    continue;
                }

                bool matched = false;
                if (op->u.open.lfn_buf[0] != '\0')
                    matched = match_lfn_name(op->u.open.lfn_buf, path + comp_start, comp_len);
                if (!matched)
                    matched = match_83_name(de->name, path + comp_start, comp_len);

                op->u.open.lfn_buf[0] = '\0';

                if (matched) {
                    serial_write("fs: matched dir entry\n", 23);
                    op->u.open.de = *de;
                    op->u.open.de_cluster = op->u.open.current_cluster;

                    // Advance to next component
                    int next_start = comp_start + comp_len;
                    if (path[next_start] == '/') next_start++;
                    op->u.open.comp_start = next_start;
                    op->u.open.comp_len = 0;
                    const char *np = path + next_start;
                    while (*np && *np != '/') { np++; op->u.open.comp_len++; }
                    op->u.open.entry_idx = 0;

                    // If more components, descend into directory
                    if (op->u.open.comp_len > 0) {
                        if (!(de->attr & 0x10)) {
                            op_complete(op, -ENOTDIR, 0);
                            return;
                        }
                        uint32_t next_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
                        if (next_cluster == 0) next_cluster = root_cluster;
                        op->u.open.current_cluster = next_cluster;
                        op->u.open.lfn_buf[0] = '\0';
                        break; // continue outer while with new cluster
                    }
                    // No more components — we found the target
                    // Jump back to top where comp_len==0 triggers completion
                    goto open_completion_check;
                }
            }

            if (op->u.open.entry_idx >= entries) {
                // Move to next cluster in chain
                uint32_t next = fat_read_entry_cached(op->u.open.current_cluster);
                if (next == 0xFFFFFFFF) {
                    // FAT miss
                    uint32_t fat_offset = op->u.open.current_cluster * 4;
                    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                    if (fat_cache_read_async(fat_sector, op) < 0) return;
                    continue;
                }
                if (next >= 0x0FFFFFF8) {
                    // End of directory chain — entry not found
                    op_complete(op, -ENOENT, 0);
                    return;
                }
                op->u.open.current_cluster = next;
                op->u.open.entry_idx = 0;
            }
        }
    }
}

// ---- STAT ----
// Reuses open logic
static void start_stat(pending_op *op, const char *path) {
    op->type = OP_STAT;
    op->u.open.de.attr = 0;
    start_open(op, NULL, path, 0);
}

// ---- READDIR ----
static void start_readdir(pending_op *op, const char *path,
                            uint32_t rd_offset, uint32_t rd_count) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = 0; resp->fd = 0; resp->file_size = 0; resp->count = 0; resp->total = 0;

    // First resolve path to directory
    for (int i = 0; i < 256 && path[i]; i++) op->u.readdir.path[i] = path[i];
    op->u.readdir.rd_offset = rd_offset;
    op->u.readdir.rd_count = rd_count;
    op->u.readdir.lfn_buf[0] = '\0';
    op->u.readdir.entry_count = 0;
    op->u.readdir.out_count = 0;
    op->u.readdir.entry_idx = 0;

    if (rd_count == 0) rd_count = 30;
    uint32_t max_entries = MAX_REPLY_DATA / sizeof(fs_dirent);
    if (rd_count > max_entries) rd_count = max_entries;
    op->u.readdir.rd_count = rd_count;

    // Resolve path first using open logic
    // For simplicity, resolve path synchronously from cache (most paths will be cached after open)
    fat_dir_entry de;
    uint32_t dir_cluster;
    // Try sync resolve (cache-only). If fails, we'd need async resolve.
    // For now, assume path is cached (common case: ls after cd)
    // TODO: full async path resolution for readdir
    bool found = false;

    // Quick sync path resolution from cache
    {
        uint32_t dc = root_cluster;
        const char *p = path;
        if (*p != '/') { op_complete(op, -ENOENT, 0); return; }
        if (*(p+1) == '\0') {
            de.attr = 0x10;
            de.fst_clus_hi = (root_cluster >> 16) & 0xFFFF;
            de.fst_clus_lo = root_cluster & 0xFFFF;
            de.file_size = 0;
            dir_cluster = root_cluster;
            found = true;
        } else {
            p++;
            while (*p) {
                const char *cs = p;
                int cl = 0;
                while (*p && *p != '/') { p++; cl++; }
                if (*p == '/') p++;

                // Find entry in dc (cache only)
                char lb[256]; lb[0] = '\0';
                uint32_t cur = dc;
                bool comp_found = false;
                while (cur >= 2 && cur < 0x0FFFFFF8) {
                    int sl = cache_lookup(cur);
                    if (sl < 0) break;
                    uint8_t *d = cache[sl].data;
                    int ent = bytes_per_cluster / 32;
                    for (int i = 0; i < ent; i++) {
                        fat_dir_entry *fde = (fat_dir_entry *)(d + i * 32);
                        if (fde->name[0] == 0x00) break;
                        if (fde->name[0] == 0xE5) { lb[0] = '\0'; continue; }
                        if (fde->attr == 0x0F) { collect_lfn_entry(fde, lb); continue; }
                        bool m = false;
                        if (lb[0] != '\0') m = match_lfn_name(lb, cs, cl);
                        if (!m) m = match_83_name(fde->name, cs, cl);
                        lb[0] = '\0';
                        if (m) {
                            de = *fde;
                            comp_found = true;
                            break;
                        }
                    }
                    if (comp_found) break;
                    cur = fat_read_entry_cached(cur);
                    if (cur == 0xFFFFFFFF) break;
                }
                if (!comp_found) break;

                if (*p == '\0') {
                    dir_cluster = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
                    if (dir_cluster == 0) dir_cluster = root_cluster;
                    found = true;
                    break;
                }
                if (!(de.attr & 0x10)) break;
                dc = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
                if (dc == 0) dc = root_cluster;
            }
        }
    }

    if (!found || !(de.attr & 0x10)) {
        op_complete(op, found ? -ENOTDIR : -ENOENT, 0);
        return;
    }

    op->u.readdir.current_cluster = dir_cluster;
    op->u.readdir.dir_cluster = dir_cluster;
    op->resume = resume_readdir;
    resume_readdir(op);
}

static void resume_readdir(pending_op *op) {
    file_resp *resp = (file_resp *)op->reply_buf;
    fs_dirent *out = (fs_dirent *)resp->data;

    while (op->u.readdir.current_cluster >= 2 && op->u.readdir.current_cluster < 0x0FFFFFF8) {
        int slot = read_cluster_async(op->u.readdir.current_cluster, op);
        if (slot < 0) return;

        uint8_t *data = cache[slot].data;
        int entries = bytes_per_cluster / 32;

        for (; op->u.readdir.entry_idx < entries; op->u.readdir.entry_idx++) {
            fat_dir_entry *fde = (fat_dir_entry *)(data + op->u.readdir.entry_idx * 32);
            if (fde->name[0] == 0x00) goto readdir_done;
            if (fde->name[0] == 0xE5) { op->u.readdir.lfn_buf[0] = '\0'; continue; }
            if (fde->attr == 0x0F) {
                collect_lfn_entry(fde, op->u.readdir.lfn_buf);
                continue;
            }
            if (fde->name[0] == '.') {
                op->u.readdir.lfn_buf[0] = '\0';
                continue;
            }

            if (op->u.readdir.entry_count >= op->u.readdir.rd_offset) {
                fs_dirent *d = &out[op->u.readdir.out_count];
                if (op->u.readdir.lfn_buf[0] != '\0') {
                    int j = 0;
                    while (op->u.readdir.lfn_buf[j] && j < 255) {
                        d->name[j] = op->u.readdir.lfn_buf[j];
                        j++;
                    }
                    d->name[j] = '\0';
                } else {
                    convert_83_to_name(fde->name, fde->nt_res, d->name, 256);
                }
                d->size = fde->file_size;
                d->date = fde->wrt_date;
                d->time = fde->wrt_time;
                d->attr = fde->attr;
                op->u.readdir.out_count++;
                if (op->u.readdir.out_count >= op->u.readdir.rd_count) goto readdir_done;
            }
            op->u.readdir.entry_count++;
            op->u.readdir.lfn_buf[0] = '\0';
        }

        // Next cluster
        uint32_t next = fat_read_entry_cached(op->u.readdir.current_cluster);
        if (next == 0xFFFFFFFF) {
            uint32_t fat_offset = op->u.readdir.current_cluster * 4;
            uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
            if (fat_cache_read_async(fat_sector, op) < 0) return;
            continue;
        }
        op->u.readdir.current_cluster = next;
        op->u.readdir.entry_idx = 0;
    }

readdir_done:
    resp->status = 0;
    resp->total = op->u.readdir.out_count;
    resp->count = op->u.readdir.out_count * sizeof(fs_dirent);
    op_complete(op, 0, resp->count);
}

// ---- RAW_READ ----
static void raw_read_complete(disk_io *io);

static void start_raw_read(pending_op *op, uint32_t lba, uint32_t count) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = 0; resp->fd = 0; resp->file_size = 0; resp->count = 0; resp->total = 0;

    uint32_t max_sectors = MAX_REPLY_DATA / 512;
    if (count > max_sectors) count = max_sectors;
    if (count == 0) count = 1;

    op->u.raw_read.lba = lba;
    op->u.raw_read.count = count;

    op->io.lba = lba;
    op->io.count = count;
    op->io.dest = resp->data;
    op->io.complete = raw_read_complete;
    op->io.ctx = op;
    op->io.next = NULL;
    op->io_active = true;
    submit_disk_io(&op->io);
}

static void raw_read_complete(disk_io *io) {
    pending_op *op = (pending_op *)io->ctx;
    file_resp *resp = (file_resp *)op->reply_buf;
    op->io_active = false;
    resp->status = 0;
    resp->count = op->u.raw_read.count * 512;
    op_complete(op, 0, resp->count);
}

// ---- Sync helpers for create/mkdir ----
// These use the sync disk I/O and are called from within the event loop
// during create/mkdir processing. They block the event loop briefly but
// writes are single-sector operations that complete quickly.

static uint32_t next_free_hint = 2;

static bool is_valid_83(const char *name, int name_len) {
    if (name_len == 0 || name_len > 12) return false;
    int dot_pos = -1;
    for (int i = 0; i < name_len; i++) {
        if (name[i] == '.') {
            if (dot_pos >= 0) return false;
            dot_pos = i;
        }
    }
    int base_len, ext_len;
    if (dot_pos < 0) { base_len = name_len; ext_len = 0; }
    else {
        base_len = dot_pos;
        ext_len = name_len - dot_pos - 1;
        if (base_len == 0 || ext_len > 3) return false;
    }
    if (base_len > 8) return false;
    for (int i = 0; i < name_len; i++) {
        char c = name[i];
        if (c == '.') continue;
        if (c >= 'a' && c <= 'z') return false;
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c == ' ' || c == '!' || c == '#' || c == '$' || c == '%' ||
            c == '&' || c == '\'' || c == '(' || c == ')' || c == '-' ||
            c == '@' || c == '^' || c == '_' || c == '`' || c == '{' ||
            c == '}' || c == '~') continue;
        return false;
    }
    return true;
}

static bool resolve_path_sync(const char *path, fat_dir_entry *out, uint32_t *out_cluster) {
    if (path[0] != '/') return false;
    uint32_t dir_cluster = root_cluster;
    if (path[1] == '\0') {
        out->attr = 0x10;
        out->fst_clus_hi = (root_cluster >> 16) & 0xFFFF;
        out->fst_clus_lo = root_cluster & 0xFFFF;
        out->file_size = 0;
        *out_cluster = root_cluster;
        return true;
    }
    const char *p = path + 1;
    while (*p) {
        const char *cs = p;
        int cl = 0;
        while (*p && *p != '/') { p++; cl++; }
        if (*p == '/') p++;
        char lb[256]; lb[0] = '\0';
        uint32_t cur = dir_cluster;
        bool found = false;
        while (cur >= 2 && cur < 0x0FFFFFF8) {
            int sl = read_cluster_sync(cur);
            if (sl < 0) return false;
            uint8_t *data = cache[sl].data;
            int ent = bytes_per_cluster / 32;
            for (int i = 0; i < ent; i++) {
                fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
                if (de->name[0] == 0x00) return false;
                if (de->name[0] == 0xE5) { lb[0] = '\0'; continue; }
                if (de->attr == 0x0F) { collect_lfn_entry(de, lb); continue; }
                bool m = false;
                if (lb[0] != '\0') m = match_lfn_name(lb, cs, cl);
                if (!m) m = match_83_name(de->name, cs, cl);
                lb[0] = '\0';
                if (m) { *out = *de; found = true; break; }
            }
            if (found) break;
            cur = fat_read_entry_sync(cur);
        }
        if (!found) return false;
        if (*p == '\0') {
            *out_cluster = ((uint32_t)out->fst_clus_hi << 16) | out->fst_clus_lo;
            if (*out_cluster == 0) *out_cluster = root_cluster;
            return true;
        }
        if (!(out->attr & 0x10)) return false;
        dir_cluster = ((uint32_t)out->fst_clus_hi << 16) | out->fst_clus_lo;
        if (dir_cluster == 0) dir_cluster = root_cluster;
    }
    return false;
}

static bool resolve_parent_sync(const char *path, uint32_t *parent_cluster,
                                 char *leaf_name, int leaf_buf_size) {
    if (path[0] != '/') return false;
    int last_slash = -1;
    int path_len = 0;
    while (path[path_len]) {
        if (path[path_len] == '/') last_slash = path_len;
        path_len++;
    }
    if (path_len == 1) return false;
    const char *ls = path + last_slash + 1;
    int ll = 0;
    while (ls[ll] && ll < leaf_buf_size - 1) { leaf_name[ll] = ls[ll]; ll++; }
    leaf_name[ll] = '\0';
    if (last_slash == 0) { *parent_cluster = root_cluster; return true; }
    char pp[256];
    for (int i = 0; i < last_slash; i++) pp[i] = path[i];
    pp[last_slash] = '\0';
    fat_dir_entry pde;
    uint32_t pc;
    if (!resolve_path_sync(pp, &pde, &pc)) return false;
    if (!(pde.attr & 0x10)) return false;
    *parent_cluster = pc;
    return true;
}

static uint32_t find_free_cluster_sync() {
    for (uint32_t sector = 0; sector < spf32; sector++) {
        uint32_t abs_sector = ((next_free_hint / 128) + sector) % spf32;
        int slot = fat_cache_read_sync(fat_start_lba + abs_sector);
        if (slot < 0) continue;
        uint8_t *fd = fat_cache[slot].data;
        for (int i = 0; i < 128; i++) {
            uint32_t c = abs_sector * 128 + i;
            if (c < 2 || c >= total_data_clusters + 2) continue;
            uint32_t e = (uint32_t)fd[i*4] | ((uint32_t)fd[i*4+1] << 8) |
                          ((uint32_t)fd[i*4+2] << 16) | ((uint32_t)fd[i*4+3] << 24);
            e &= 0x0FFFFFFF;
            if (e == 0) {
                next_free_hint = c + 1;
                if (next_free_hint >= total_data_clusters + 2) next_free_hint = 2;
                return c;
            }
        }
    }
    return 0;
}

static uint32_t allocate_cluster_sync() {
    uint32_t c = find_free_cluster_sync();
    if (c == 0) return 0;
    uint32_t fat_offset = c * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;
    int slot = fat_cache_read_sync(fat_sector);
    if (slot < 0) return 0;
    uint8_t sbuf[512];
    __memcpy(sbuf, fat_cache[slot].data, 512);
    uint8_t *p = sbuf + offset_in_sector;
    uint32_t old = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
    uint32_t nv = (old & 0xF0000000) | 0x0FFFFFFF;
    p[0]=nv&0xFF; p[1]=(nv>>8)&0xFF; p[2]=(nv>>16)&0xFF; p[3]=(nv>>24)&0xFF;
    disk_write_sync(fat_sector, 1, sbuf);
    disk_write_sync(fat_sector + spf32, 1, sbuf);
    __memcpy(fat_cache[slot].data, sbuf, 512);
    return c;
}

static int dir_chain_extend_sync(uint32_t dir_cluster, uint32_t *new_cluster_out) {
    uint32_t nc = allocate_cluster_sync();
    if (nc == 0) return 4;
    uint8_t zbuf[4096];
    for (int i = 0; i < (int)bytes_per_cluster; i++) zbuf[i] = 0;
    uint32_t nlba = data_start_lba + (nc - 2) * sectors_per_cluster;
    if (disk_write_sync(nlba, sectors_per_cluster, zbuf) != 0) return 5;
    uint32_t cur = dir_cluster, prev = cur;
    while (cur >= 2 && cur < 0x0FFFFFF8) { prev = cur; cur = fat_read_entry_sync(cur); }
    uint32_t fat_offset = prev * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;
    int slot = fat_cache_read_sync(fat_sector);
    if (slot < 0) return 6;
    uint8_t sbuf[512];
    __memcpy(sbuf, fat_cache[slot].data, 512);
    uint8_t *p = sbuf + offset_in_sector;
    uint32_t old = (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    uint32_t nval = (old & 0xF0000000) | (nc & 0x0FFFFFFF);
    p[0]=nval&0xFF; p[1]=(nval>>8)&0xFF; p[2]=(nval>>16)&0xFF; p[3]=(nval>>24)&0xFF;
    disk_write_sync(fat_sector, 1, sbuf);
    disk_write_sync(fat_sector + spf32, 1, sbuf);
    __memcpy(fat_cache[slot].data, sbuf, 512);
    cache_invalidate(dir_cluster);
    *new_cluster_out = nc;
    return 0;
}

static bool find_free_dir_slots_sync(uint32_t dir_cluster, int needed, dir_slot *slots_out) {
    uint32_t cluster = dir_cluster;
    int epc = bytes_per_cluster / 32;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        int cs = read_cluster_sync(cluster);
        if (cs < 0) return false;
        uint8_t *data = cache[cs].data;
        int run_start = -1, run_len = 0;
        for (int i = 0; i < epc; i++) {
            fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
            bool is_free = (de->name[0] == 0x00 || de->name[0] == 0xE5);
            if (is_free) {
                if (run_len == 0) run_start = i;
                run_len++;
                if (run_len >= needed) {
                    for (int j = 0; j < needed; j++) {
                        slots_out[j].cluster = cluster;
                        slots_out[j].index = run_start + j;
                    }
                    return true;
                }
            } else {
                run_len = 0;
            }
        }
        uint32_t next = fat_read_entry_sync(cluster);
        if (next >= 0x0FFFFFF8) {
            uint32_t nc;
            int rc = dir_chain_extend_sync(dir_cluster, &nc);
            if (rc != 0) return false;
            for (int j = 0; j < needed; j++) {
                slots_out[j].cluster = nc;
                slots_out[j].index = j;
            }
            return true;
        }
        cluster = next;
    }
    return false;
}

static int write_dir_entry_at_sync(uint32_t cluster, int index, const fat_dir_entry *entry) {
    int cs = read_cluster_sync(cluster);
    if (cs < 0) return 1;
    uint8_t *data = cache[cs].data;
    __memcpy(data + index * 32, entry, 32);
    int eo = index * 32;
    int sic = eo / 512;
    uint32_t clba = data_start_lba + (cluster - 2) * sectors_per_cluster;
    if (disk_write_sync(clba + sic, 1, data + sic * 512) != 0) return 2;
    int ois = eo % 512;
    if (ois + 32 > 512 && sic + 1 < (int)sectors_per_cluster) {
        if (disk_write_sync(clba + sic + 1, 1, data + (sic + 1) * 512) != 0) return 2;
    }
    return 0;
}

static void write_lfn_entries_sync(uint32_t dir_cluster, const dir_slot *slots,
                                     int lfn_count, const char *name, int name_len,
                                     const char short_name[11]) {
    uint8_t chk = lfn_checksum((const uint8_t *)short_name);
    for (int n = 0; n < lfn_count; n++) {
        int seq = lfn_count - n;
        bool is_last = (n == 0);
        fat_dir_entry lfn_de;
        uint8_t *raw = (uint8_t *)&lfn_de;
        for (int k = 0; k < 32; k++) raw[k] = 0;
        raw[0] = seq | (is_last ? 0x40 : 0x00);
        raw[11] = 0x0F; raw[12] = 0x00; raw[13] = chk;
        static const int offsets[] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
        int base = n * 13;
        for (int c = 0; c < 13; c++) {
            int pos = base + c;
            uint16_t ucs2;
            if (pos < name_len) ucs2 = (uint16_t)(uint8_t)name[pos];
            else if (pos == name_len) ucs2 = 0x0000;
            else ucs2 = 0xFFFF;
            raw[offsets[c]] = ucs2 & 0xFF;
            raw[offsets[c] + 1] = (ucs2 >> 8) & 0xFF;
        }
        write_dir_entry_at_sync(dir_cluster, slots[n].index, &lfn_de);
    }
}

static bool find_dir_entry_sync(uint32_t dir_cluster, const char *name,
                                 int name_len, fat_dir_entry *out) {
    char lb[256]; lb[0] = '\0';
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        int slot = read_cluster_sync(cluster);
        if (slot < 0) return false;
        uint8_t *data = cache[slot].data;
        int entries = bytes_per_cluster / 32;
        for (int i = 0; i < entries; i++) {
            fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
            if (de->name[0] == 0x00) return false;
            if (de->name[0] == 0xE5) { lb[0] = '\0'; continue; }
            if (de->attr == 0x0F) { collect_lfn_entry(de, lb); continue; }
            bool m = false;
            if (lb[0] != '\0') m = match_lfn_name(lb, name, name_len);
            if (!m) m = match_83_name(de->name, name, name_len);
            if (m) { *out = *de; return true; }
            lb[0] = '\0';
        }
        cluster = fat_read_entry_sync(cluster);
    }
    return false;
}

static int generate_short_name(const char *name, int name_len, char out[11],
                                uint32_t dir_cluster) {
    if (is_valid_83(name, name_len)) {
        format_83_name(name, name_len, out);
        return 0;
    }

    char basename[9] = {};
    char extname[4] = {};
    int dot_pos = -1;
    for (int i = name_len - 1; i >= 0; i--) {
        if (name[i] == '.') { dot_pos = i; break; }
    }
    int blen = (dot_pos >= 0) ? dot_pos : name_len;
    if (blen > 6) blen = 6;
    for (int i = 0; i < blen; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
        basename[i] = c;
    }
    if (dot_pos >= 0) {
        int elen = name_len - dot_pos - 1;
        if (elen > 3) elen = 3;
        for (int i = 0; i < elen; i++) {
            char c = name[dot_pos + 1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
            extname[i] = c;
        }
    }

    for (int n = 1; n <= 99; n++) {
        char candidate[11];
        for (int i = 0; i < 11; i++) candidate[i] = ' ';
        int tl = (n >= 10) ? 3 : 2;
        int bc = 8 - tl;
        if (bc > 6) bc = 6;
        int pos = 0;
        for (int i = 0; i < bc && basename[i]; i++) candidate[pos++] = basename[i];
        candidate[pos++] = '~';
        if (n >= 10) { candidate[pos++] = '0' + (n / 10); candidate[pos++] = '0' + (n % 10); }
        else candidate[pos++] = '0' + n;
        for (int i = 0; i < 3 && extname[i]; i++) candidate[8 + i] = extname[i];
        fat_dir_entry dummy;
        if (!find_dir_entry_sync(dir_cluster, candidate, 11, &dummy)) {
            for (int i = 0; i < 11; i++) out[i] = candidate[i];
            return n;
        }
    }
    format_83_name("UNKNO~1", 7, out);
    return 1;
}

// ---- CREATE ----
// Multi-phase: resolve_parent → check existing → gen_short_name → find_slots → write
// For simplicity, use sync I/O for the write phases (they're fast: single sector writes)
// The main disk I/O that benefits from async is the path resolution and dir scan.

static void start_create(pending_op *op, const char *path) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = 0; resp->fd = 0; resp->file_size = 0; resp->count = 0; resp->total = 0;

    // Resolve path — if exists, update timestamp
    fat_dir_entry existing;
    uint32_t existing_cluster;
    if (resolve_path_sync(path, &existing, &existing_cluster)) {
        // Exists — update timestamp (sync, single sector write)
        uint32_t parent_cluster;
        char leaf_name[256];
        if (!resolve_parent_sync(path, &parent_cluster, leaf_name, 256)) {
            op_complete(op, -ENOENT, 0);
            return;
        }
        int leaf_len = 0;
        while (leaf_name[leaf_len]) leaf_len++;

        // Find and update the entry
        uint32_t cluster = parent_cluster;
        char lb[256]; lb[0] = '\0';
        while (cluster >= 2 && cluster < 0x0FFFFFF8) {
            int cs = read_cluster_sync(cluster);
            if (cs < 0) { op_complete(op, -EIO, 0); return; }
            uint8_t *data = cache[cs].data;
            int ent = bytes_per_cluster / 32;
            for (int i = 0; i < ent; i++) {
                fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
                if (de->name[0] == 0x00) break;
                if (de->name[0] == 0xE5) { lb[0] = '\0'; continue; }
                if (de->attr == 0x0F) { collect_lfn_entry(de, lb); continue; }
                bool m = false;
                if (lb[0] != '\0') m = match_lfn_name(lb, leaf_name, leaf_len);
                if (!m) m = match_83_name(de->name, leaf_name, leaf_len);
                if (m) {
                    de->wrt_date = HARD_DATE;
                    de->wrt_time = HARD_TIME;
                    de->lst_acc_date = HARD_DATE;
                    int eo = i * 32;
                    int sic = eo / 512;
                    uint32_t clba = data_start_lba + (cluster - 2) * sectors_per_cluster;
                    disk_write_sync(clba + sic, 1, data + sic * 512);
                    op_complete(op, 0, 0);
                    return;
                }
                lb[0] = '\0';
            }
            cluster = fat_read_entry_sync(cluster);
        }
        op_complete(op, -ENOENT, 0);
        return;
    }

    // File doesn't exist — create new
    uint32_t parent_cluster;
    char leaf_name[256];
    if (!resolve_parent_sync(path, &parent_cluster, leaf_name, 256)) {
        op_complete(op, -ENOENT, 0);
        return;
    }

    int leaf_len = 0;
    while (leaf_name[leaf_len]) leaf_len++;

    char short_name[11];
    generate_short_name(leaf_name, leaf_len, short_name, parent_cluster);

    int lfn_count = (leaf_len + 12) / 13;
    int total_slots = lfn_count + 1;

    dir_slot slots[20];
    if (!find_free_dir_slots_sync(parent_cluster, total_slots, slots)) {
        op_complete(op, -ENOMEM, 0);
        return;
    }

    if (lfn_count > 0) {
        write_lfn_entries_sync(parent_cluster, slots, lfn_count, leaf_name, leaf_len, short_name);
    }

    fat_dir_entry new_entry;
    __memcpy(new_entry.name, short_name, 11);
    new_entry.attr = 0x20;
    new_entry.nt_res = 0;
    new_entry.crt_time_tenth = 0;
    new_entry.crt_time = HARD_TIME;
    new_entry.crt_date = HARD_DATE;
    new_entry.lst_acc_date = HARD_DATE;
    new_entry.fst_clus_hi = 0;
    new_entry.wrt_time = HARD_TIME;
    new_entry.wrt_date = HARD_DATE;
    new_entry.fst_clus_lo = 0;
    new_entry.file_size = 0;

    int rc = write_dir_entry_at_sync(slots[lfn_count].cluster, slots[lfn_count].index, &new_entry);
    op_complete(op, (rc != 0) ? -rc : 0, 0);
}

// ---- MKDIR ----
static void start_mkdir(pending_op *op, const char *path) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = 0; resp->fd = 0; resp->file_size = 0; resp->count = 0; resp->total = 0;

    fat_dir_entry existing;
    uint32_t existing_cluster;
    if (resolve_path_sync(path, &existing, &existing_cluster)) {
        op_complete(op, -EEXIST, 0);
        return;
    }

    uint32_t parent_cluster;
    char leaf_name[256];
    if (!resolve_parent_sync(path, &parent_cluster, leaf_name, 256)) {
        op_complete(op, -ENOENT, 0);
        return;
    }

    int leaf_len = 0;
    while (leaf_name[leaf_len]) leaf_len++;

    uint32_t new_dir_cluster = allocate_cluster_sync();
    if (new_dir_cluster == 0) {
        op_complete(op, -ENOMEM, 0);
        return;
    }

    uint8_t cluster_data_buf[4096];
    for (int i = 0; i < (int)bytes_per_cluster; i++) cluster_data_buf[i] = 0;

    fat_dir_entry dot_entry;
    dot_entry.name[0] = '.'; for (int i = 1; i < 11; i++) dot_entry.name[i] = ' ';
    dot_entry.attr = 0x10; dot_entry.nt_res = 0; dot_entry.crt_time_tenth = 0;
    dot_entry.crt_time = HARD_TIME; dot_entry.crt_date = HARD_DATE;
    dot_entry.lst_acc_date = HARD_DATE;
    dot_entry.fst_clus_hi = (new_dir_cluster >> 16) & 0xFFFF;
    dot_entry.wrt_time = HARD_TIME; dot_entry.wrt_date = HARD_DATE;
    dot_entry.fst_clus_lo = new_dir_cluster & 0xFFFF;
    dot_entry.file_size = 0;
    __memcpy(cluster_data_buf, &dot_entry, 32);

    fat_dir_entry dotdot_entry;
    dotdot_entry.name[0] = '.'; dotdot_entry.name[1] = '.';
    for (int i = 2; i < 11; i++) dotdot_entry.name[i] = ' ';
    dotdot_entry.attr = 0x10; dotdot_entry.nt_res = 0; dotdot_entry.crt_time_tenth = 0;
    dotdot_entry.crt_time = HARD_TIME; dotdot_entry.crt_date = HARD_DATE;
    dotdot_entry.lst_acc_date = HARD_DATE;
    dotdot_entry.fst_clus_hi = (parent_cluster >> 16) & 0xFFFF;
    dotdot_entry.wrt_time = HARD_TIME; dotdot_entry.wrt_date = HARD_DATE;
    dotdot_entry.fst_clus_lo = parent_cluster & 0xFFFF;
    dotdot_entry.file_size = 0;
    __memcpy(cluster_data_buf + 32, &dotdot_entry, 32);

    uint32_t new_dir_lba = data_start_lba + (new_dir_cluster - 2) * sectors_per_cluster;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        disk_write_sync(new_dir_lba + s, 1, cluster_data_buf + s * 512);
    }

    char short_name[11];
    generate_short_name(leaf_name, leaf_len, short_name, parent_cluster);

    int lfn_count = (leaf_len + 12) / 13;
    int total_slots = lfn_count + 1;

    dir_slot slots[20];
    if (!find_free_dir_slots_sync(parent_cluster, total_slots, slots)) {
        op_complete(op, -ENOMEM, 0);
        return;
    }

    if (lfn_count > 0) {
        write_lfn_entries_sync(parent_cluster, slots, lfn_count, leaf_name, leaf_len, short_name);
    }

    fat_dir_entry new_entry;
    __memcpy(new_entry.name, short_name, 11);
    new_entry.attr = 0x10; new_entry.nt_res = 0; new_entry.crt_time_tenth = 0;
    new_entry.crt_time = HARD_TIME; new_entry.crt_date = HARD_DATE;
    new_entry.lst_acc_date = HARD_DATE;
    new_entry.fst_clus_hi = (new_dir_cluster >> 16) & 0xFFFF;
    new_entry.wrt_time = HARD_TIME; new_entry.wrt_date = HARD_DATE;
    new_entry.fst_clus_lo = new_dir_cluster & 0xFFFF;
    new_entry.file_size = 0;

    int rc = write_dir_entry_at_sync(slots[lfn_count].cluster, slots[lfn_count].index, &new_entry);
    op_complete(op, (rc != 0) ? -rc : 0, 0);
}

// ===================== FAT32 init (sync) =====================
static void fat32_init() {
    disk_read_sync(0, 1);
    uint8_t *mbr = (uint8_t *)dresp->data;

    part_start_lba = 0;
    uint32_t part_total_sectors = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *entry = mbr + 0x1BE + i * 16;
        uint8_t ptype = entry[4];
        if (ptype == 0x0B || ptype == 0x0C) {
            part_start_lba = (uint32_t)entry[8]  | ((uint32_t)entry[9] << 8) |
                             ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
            part_total_sectors = (uint32_t)entry[12] | ((uint32_t)entry[13] << 8) |
                                 ((uint32_t)entry[14] << 16) | ((uint32_t)entry[15] << 24);
            break;
        }
    }

    serial_write("fs: part_start_lba found\n", 26);

    disk_read_sync(part_start_lba, 1);
    uint8_t *bpb = (uint8_t *)dresp->data;

    uint16_t bps = (uint16_t)bpb[11] | ((uint16_t)bpb[12] << 8);
    sectors_per_cluster = bpb[13];
    uint16_t reserved = (uint16_t)bpb[14] | ((uint16_t)bpb[15] << 8);
    spf32 = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
            ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    root_cluster = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                   ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);

    // Debug: print key BPB values
    char dbg[64];
    int pos = 0;
    const char prefix[] = "bps=X sc=X spf=X rc=X\n";
    // Quick hex print
    serial_write("fs: bpb first 4 bytes: ", 23);
    for (int i = 0; i < 4; i++) {
        char hex[3];
        hex[0] = "0123456789ABCDEF"[(bpb[i] >> 4) & 0xF];
        hex[1] = "0123456789ABCDEF"[bpb[i] & 0xF];
        hex[2] = 0;
        serial_write(hex, 2);
    }
    serial_write("\n", 1);

    if (bps != 512 || sectors_per_cluster == 0 || spf32 == 0 || root_cluster < 2) {
        const char msg[] = "EBPB\n";
        serial_write(msg, sizeof(msg) - 1);
        while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    }

    fat_start_lba = part_start_lba + reserved;
    data_start_lba = fat_start_lba + spf32 * 2;
    bytes_per_cluster = sectors_per_cluster * 512;

    uint32_t data_sectors = part_total_sectors - (data_start_lba - part_start_lba);
    total_data_clusters = data_sectors / sectors_per_cluster;

    for (uint32_t s = 0; s < spf32 && s < FAT_CACHE_PAGES; s++) {
        fat_cache_read_sync(fat_start_lba + s);
    }
}

// ===================== Main event loop =====================
extern "C" void _start() {
    serial_write("fs_driver: started\n", 20);
    while ((disk_driver_pid = device_lookup(DEV_DISK)) < 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 10);
    }
    void *disk_shm_ptr = NULL;
    while (shm_attach(disk_driver_pid, &disk_shm_ptr) < 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 10);
    }
    uint64_t disk_shm = (uint64_t)disk_shm_ptr;
    disk_hdr = (volatile disk_shm_header *)(disk_shm + DISK_SHM_HEADER_OFFSET);
    dreq     = (volatile disk_req_shm *)(disk_shm + DISK_REQ_OFFSET);
    dresp    = (volatile disk_resp_shm *)(disk_shm + DISK_RESP_OFFSET);
    disk_hdr->fs_driver_pid = getpid();

    // Initialize caches
    for (int i = 0; i < CACHE_SLOTS; i++) {
        cache[i].cluster = 0xFFFFFFFF;
        cache[i].age = 0;
        cache[i].pin_count = 0;
    }
    for (int i = 0; i < FAT_CACHE_PAGES; i++) {
        fat_cache[i].sector_lba = 0xFFFFFFFF;
        fat_cache_age[i] = 0;
    }

    // Initialize sessions
    for (int i = 0; i < MAX_CLIENTS; i++) {
        sessions[i].client_pid = -1;
    }

    // Initialize pending_op pool
    for (int i = 0; i < MAX_PENDING_OPS; i++) {
        pending_pool[i].client_pid = -1;
    }

    // Initialize disk_io pool
    for (int i = 0; i < DISK_IO_POOL_SIZE; i++) {
        disk_io_used[i] = false;
    }

    // Sync init (MBR/BPB/FAT cache warm-up)
    serial_write("fs_driver: calling fat32_init\n", 30);
    fat32_init();
    serial_write("fs_driver: fat32_init done\n", 27);

    device_register(getpid(), DEV_FS);
    serial_write("fs_driver: registered DEV_FS, entering event loop\n", 50);

    // Event loop
    while (1) {
        struct recv_msg m;
        uint8_t data_buf[sizeof(struct file_req)];
        int rr = recv(&m, data_buf, sizeof(data_buf), 0);
        if (rr != 0) continue;

        // Disk completion notification
        if (m.type == RECV_NOTIFY && (int32_t)m.src == disk_driver_pid) {
            handle_disk_complete();
            continue;
        }

        // Other notifications (write_lock release, etc.)
        if (m.type == RECV_NOTIFY) {
            continue;
        }

        if (m.type != RECV_MSG) continue;

        pid_t client_pid = (pid_t)m.src;

        struct file_req *freq = (struct file_req *)data_buf;

        serial_write("fs: got MSG cmd=", 17);
        char hex[3];
        hex[0] = "0123456789ABCDEF"[(freq->cmd >> 4) & 0xF];
        hex[1] = "0123456789ABCDEF"[freq->cmd & 0xF];
        hex[2] = 0;
        serial_write(hex, 2);
        serial_write(" path=", 6);
        serial_write(freq->path, 20);
        serial_write("\n", 1);

        struct client_session *sess = get_session(client_pid);
        if (!sess) {
            struct file_resp err_resp;
            err_resp.status = -ENOMEM;
            err_resp.fd = 0;
            err_resp.file_size = 0;
            err_resp.count = 0;
            err_resp.total = 0;
            msg_resp(&err_resp, sizeof(err_resp));
            continue;
        }

        // Find session index
        int sess_idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (sessions[i].client_pid == client_pid) { sess_idx = i; break; }
        }

        switch (freq->cmd) {
        case FILE_CMD_CLOSE: {
            // Close is synchronous, no disk I/O
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
            if (freq->fs_fd < MAX_SESSION_FDS) {
                sess->open_files[freq->fs_fd].used = false;
                sess->open_files[freq->fs_fd].ra_sequential = false;
            }
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_PING: {
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_WRITE: {
            struct file_resp resp;
            resp.status = -ENOSYS; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_READ: {
            pending_op *op = alloc_pending_op();
            if (!op) {
                struct file_resp resp;
                resp.status = -ENOMEM; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
                msg_resp(&resp, sizeof(resp));
                break;
            }
            op->client_pid = client_pid;
            op->session_idx = sess_idx;
            op->type = OP_READ;
            op->io_active = false;
            start_read(op, sess, freq->fs_fd, freq->offset, freq->count);
            break;
        }

        case FILE_CMD_OPEN: {
            pending_op *op = alloc_pending_op();
            if (!op) {
                struct file_resp resp;
                resp.status = -ENOMEM; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
                msg_resp(&resp, sizeof(resp));
                break;
            }
            op->client_pid = client_pid;
            op->session_idx = sess_idx;
            op->type = OP_OPEN;
            op->io_active = false;
            start_open(op, sess, freq->path, freq->flags);
            break;
        }

        case FILE_CMD_STAT: {
            pending_op *op = alloc_pending_op();
            if (!op) {
                struct file_resp resp;
                resp.status = -ENOMEM; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
                msg_resp(&resp, sizeof(resp));
                break;
            }
            op->client_pid = client_pid;
            op->session_idx = sess_idx;
            op->type = OP_STAT;
            op->io_active = false;
            start_stat(op, freq->path);
            break;
        }

        case FILE_CMD_READDIR: {
            pending_op *op = alloc_pending_op();
            if (!op) {
                struct file_resp resp;
                resp.status = -ENOMEM; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
                msg_resp(&resp, sizeof(resp));
                break;
            }
            op->client_pid = client_pid;
            op->session_idx = sess_idx;
            op->type = OP_READDIR;
            op->io_active = false;
            start_readdir(op, freq->path, freq->readdir_offset, freq->readdir_count);
            break;
        }

        case FILE_CMD_RAW_READ: {
            pending_op *op = alloc_pending_op();
            if (!op) {
                struct file_resp resp;
                resp.status = -ENOMEM; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
                msg_resp(&resp, sizeof(resp));
                break;
            }
            op->client_pid = client_pid;
            op->session_idx = sess_idx;
            op->type = OP_RAW_READ;
            op->io_active = false;
            start_raw_read(op, freq->lba, freq->count);
            break;
        }

        case FILE_CMD_CREATE: {
            pending_op *op = alloc_pending_op();
            if (!op) {
                struct file_resp resp;
                resp.status = -ENOMEM; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
                msg_resp(&resp, sizeof(resp));
                break;
            }
            op->client_pid = client_pid;
            op->session_idx = sess_idx;
            op->type = OP_CREATE;
            op->io_active = false;
            start_create(op, freq->path);
            break;
        }

        case FILE_CMD_MKDIR: {
            pending_op *op = alloc_pending_op();
            if (!op) {
                struct file_resp resp;
                resp.status = -ENOMEM; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
                msg_resp(&resp, sizeof(resp));
                break;
            }
            op->client_pid = client_pid;
            op->session_idx = sess_idx;
            op->type = OP_MKDIR;
            op->io_active = false;
            start_mkdir(op, freq->path);
            break;
        }

        default: {
            struct file_resp resp;
            resp.status = -EINVAL; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
            msg_resp(&resp, sizeof(resp));
            break;
        }
        }
    }
}
