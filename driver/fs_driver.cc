// ===================== FAT32 filesystem driver (user-space) — event loop
// Receives requests via sys_msg (variable-length IPC), performs FAT32 operations,
// replies via sys_msg_resp. Accesses disk via sys_block_read/sys_block_write.
// Single-threaded event loop: disk I/O is synchronous, client requests are
// processed via pending_op state machines with resume callbacks.
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/device.h>
#include <sys/block.h>
#include "common/shm.h"
#include "common/dev.h"
#include "common/errno.h"
#include "common/syscall.h"

// Open flags (must match user/include/fcntl.h)
#define O_WRONLY    1
#define O_RDWR      2
#define O_APPEND    8

static uint32_t next_free_hint = 2;  // forward decl, defined also near touch/mkdir

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

// Synchronous disk I/O via kernel syscalls (unified BLOCK_IO)
static int disk_read_sync(uint32_t lba, uint32_t count, void *buf) {
    return block_read(lba, buf, count);
}

static int disk_write_sync(uint32_t lba, uint32_t count, const void *data) {
    return block_write(lba, data, count);
}

// ===================== disk_io: disk I/O descriptor =====================
struct disk_io;

struct disk_io {
    uint32_t lba;
    uint32_t count;
    void *buf;                     // read: target buffer; write: source buffer
    uint8_t  dir;                  // 0=read, 1=write
    void (*complete)(disk_io *io); // completion callback
    void *ctx;                     // callback context (typically pending_op*)
    uint32_t cookie;               // async completion cookie (matches kernel RECV_NOTIFY)
    disk_io *next;                 // unused (kept for struct compatibility)
};

// Async submit: calls sys_block_async, completion arrives via RECV_NOTIFY.
// io->complete() is NOT called here — it will be called when RECV_NOTIFY arrives.
static void submit_disk_io(disk_io *io) {
    int rc = block_async(io->lba, io->buf, io->count, io->dir);
    if (rc < 0) {
        // Immediate error (queue full, etc.) — call complete with error
        io->complete(io);
        return;
    }
    io->cookie = (uint32_t)rc;
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
#define FAT_CACHE_PAGES 16

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

    slot = fat_cache_alloc(sector_lba);
    if (disk_read_sync(sector_lba, 1, fat_cache[slot].data) != 0) return -1;
    return slot;
}

// Sync cluster read (for init only)
static int read_cluster_sync(uint32_t cluster) {
    int slot = cache_lookup(cluster);
    if (slot >= 0) return slot;

    slot = cache_alloc(cluster);
    uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;

    if (disk_read_sync(lba, sectors_per_cluster, cache[slot].data) != 0) {
        cache[slot].cluster = 0xFFFFFFFF;
        return -1;
    }
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
    // Name exactly fills all 13 slots (e.g. 13-char name in single entry).
    // Null-terminate so match_lfn_name works correctly.
    lfn_buf[base + n_chars] = '\0';
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
#define FILE_CMD_UNLINK   10
#define FILE_CMD_RMDIR    11
#define FILE_CMD_FSTAT    12
#define FILE_CMD_OPENDIR  13
#define FILE_CMD_DIRENT   14
#define FILE_CMD_CLOSEDIR 15
#define FILE_CMD_SEEK     16
#define FILE_CMD_ACCESS   17

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
    OP_STAT,
    OP_WRITE,
    OP_UNLINK,
    OP_RMDIR,
    OP_FSTAT,
    OP_OPENDIR,
    OP_DIRENT
};

// Write state machine phases
#define WPHASE_ACQUIRE_LOCK  0
#define WPHASE_LOCATE        1
#define WPHASE_EXTEND_ALLOC  2
#define WPHASE_EXTEND_ZERO   3
#define WPHASE_EXTEND_LINK   4
#define WPHASE_WRITE_DATA    5
#define WPHASE_UPDATE_DIR    6
#define WPHASE_DONE          7
#define WPHASE_WRITE_NEXT    8   // check if more data/clusters after write

// FAT dual-write state machine (used by write path)
struct fat_write_state {
    uint32_t cluster;
    uint32_t value;
    uint32_t fat_sector;
    uint32_t fat2_sector;
    uint32_t offset_in_sector;
    int fat_cache_slot;
    int phase; // 0=need FAT read, 1=FAT read done write FAT1, 2=FAT1 done write FAT2, 3=done
    uint8_t sector_buf[512];
    int result;
};

// Async dir entry write state machine (used by write path)
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

// ===================== resolve_state (reusable async path resolution) =====================
#define RS_INIT          0  // Extract next path component, init scan
#define RS_READ_CLUSTER  1  // async read dir cluster (cache miss)
#define RS_SCAN_ENTRIES  2  // Scan entries (pure computation, no blocking)
#define RS_READ_FAT      3  // async read FAT (cache miss, chain traversal)
#define RS_DONE          4

// resolve_step() return values
#define RESOLVE_DONE     0
#define RESOLVE_ASYNC    1
#define RESOLVE_ERROR    (-1)  // generic error; caller can set specific errno

struct resolve_state {
    const char *path;      // pointer into parent context's path buffer
    int path_pos;          // current position in path string
    int comp_start;        // start index of current component
    int comp_len;          // length of current component
    uint32_t dir_cluster;  // cluster of directory being scanned
    uint32_t current_cluster; // cluster being scanned now
    int entry_idx;         // index within current cluster's entries
    char lfn_buf[256];     // accumulated LFN for current entry
    bool found;            // did we find the target?
    fat_dir_entry result;  // the found dir entry
    uint32_t result_cluster; // cluster where result entry was found
    int result_entry_idx;  // entry index within result_cluster
    bool is_parent;        // true=resolve to parent dir, extract leaf_name
    int is_parent_end;     // path index where parent path ends (= last_slash)
    char leaf_name[256];   // extracted leaf name (when is_parent=true)
    int leaf_len;          // length of leaf_name
    int phase;             // RS_INIT..RS_DONE
};

// ===================== find_slots_state (async dir slot scanning + chain extend) =====================
#define FS_SCAN_CLUSTER    0  // Scan current cluster entries
#define FS_READ_CLUSTER    1  // async read dir cluster (cache miss)
#define FS_READ_FAT        2  // async read FAT (chain traversal)
#define FS_EXTEND_ALLOC    3  // Chain tail not enough → allocate new cluster
#define FS_EXTEND_ZERO     4  // Zero-fill new cluster
#define FS_EXTEND_LINK     5  // Link new cluster (dir_tail_lookup find tail, miss→traverse)
#define FS_EXTEND_LINK_WAIT  8  // internal sub-phase: waiting for FAT dual-write of link
#define FS_FOUND           6  // Found free slots
#define FS_DONE            7

// find_slots_step() return values
#define FIND_SLOTS_DONE    0
#define FIND_SLOTS_ASYNC   1
#define FIND_SLOTS_ERROR   (-1)

struct find_slots_state {
    uint32_t dir_cluster;    // directory's starting cluster
    uint32_t current_cluster; // cluster being scanned
    int entry_idx;           // index within current cluster
    int needed;              // total slots needed (lfn_count + 1)
    int run_start;           // start of current free-run
    int run_len;             // length of current free-run
    dir_slot slots[20];       // output slot locations
    uint32_t new_cluster;    // newly allocated cluster (for extend)
    uint32_t tail_cluster;   // tail of dir chain (via dir_tail_lookup or traversal)
    bool tail_traversing;    // true when traversing chain to find tail (on cache miss)
    uint32_t traverse_cluster; // current cluster during tail traversal
    bool end_of_dir;         // true if allocated slots include/extend past original 0x00 marker
    int phase;               // FS_SCAN_CLUSTER..FS_DONE
};

// ===================== create_dir (main state machine, create+mkdir shared) =====================
#define CD_RESOLVE_PATH      0
#define CD_RESOLVE_PARENT    1
#define CD_ALLOCATE          2
#define CD_ALLOCATE_WAIT     9   // internal: waiting for allocate_cluster_async completion
#define CD_INIT_DIR          3
#define CD_FIND_SLOTS        4
#define CD_WRITE_LFN         5
#define CD_WRITE_SHORT       6
#define CD_UPDATE_TIMESTAMP  7
#define CD_DONE              8
#define CD_WRITE_EOD         10  // write end-of-directory 0x00 terminator

// gen_short_name async phases (embedded in create_dir_context)
#define GS_SCAN      0  // Scan entries in current cluster
#define GS_READ_CLUSTER  1  // async read cluster (cache miss)
#define GS_READ_FAT  2  // async read FAT (chain traversal)

struct create_dir_context {
    char path[256];
    bool is_mkdir;            // true for mkdir, false for create
    int phase;                // CD_RESOLVE_PATH..CD_DONE

    // Sub-state machines (only one active at a time, sharing op->io)
    resolve_state rs;
    find_slots_state fss;
    fat_write_state fws;
    write_dir_state wds;

    // Results from resolve_state
    uint32_t parent_cluster;
    char leaf_name_buf[256];  // redundant with rs.leaf_name but explicit
    int leaf_len;
    char short_name[11];      // generated 8.3 short name

    // Cluster allocation result (mkdir only)
    uint32_t new_dir_cluster;
    uint32_t allocated_cluster;

    // LFN/short name entry tracking
    int lfn_count;
    int total_slots;
    int lfn_written_count;

    // Cluster data buffer for zero-fill / init dir
    uint8_t cluster_buf[4096];

    // gen_short_name async state
    int gen_short_num;        // current tilde number being tried (1-99)
    uint32_t gen_short_cluster; // cluster being scanned for collision check
    int gen_short_entry_idx;   // entry index within current cluster
    char gen_short_lfn_buf[256]; // LFN accumulation during collision scan
    bool gen_short_collision;  // did we find a collision?
    int gen_short_phase;      // GS_SCAN, GS_READ_CLUSTER, GS_READ_FAT
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

        // OPEN + STAT (share resolve_state)
        struct {
            char path[256];
            uint32_t flags;             // open flags (stat uses 0)
            resolve_state rs;           // path resolution sub-state machine
        } open;

        // READDIR
        struct {
            char path[256];
            resolve_state rs;           // path resolution sub-state machine
            uint32_t rd_offset;
            uint32_t rd_count;
            uint32_t dir_cluster;       // resolved dir cluster
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

        // CREATE_DIR (unified create + mkdir)
        create_dir_context create_dir;

        // WRITE
        struct {
            uint32_t fs_fd;
            uint32_t flags;
            uint64_t offset;
            uint32_t count;
            uint32_t bytes_written;
            uint32_t current_cluster;
            uint32_t chain_pos;
            uint32_t offset_clusters;
            uint32_t in_cluster_offset;
            uint32_t orig_file_size;
            uint32_t new_file_size;
            int phase;
            fat_write_state fws;
            write_dir_state wds;
            uint8_t cluster_buf[4096];
            uint32_t tail_cluster;
            uint32_t allocated_cluster;
            uint32_t write_data_len;    // length of write data in write_data_bufs
            int      pool_idx;          // index into write_data_bufs
            bool     link_started;      // true after EXTEND_LINK FAT write has been initiated
        } write;
    } u;

    // Per-op reply buffer
    uint8_t reply_buf[sizeof(file_resp) + 65536];
};

#define MAX_PENDING_OPS 16
static pending_op pending_pool[MAX_PENDING_OPS];

// Write data buffer pool (indexed by pending_op slot index)
static uint8_t write_data_bufs[MAX_PENDING_OPS][65536];

// Dir chain tail pointer cache
#define DIR_TAIL_CACHE_SIZE 64
struct dir_tail_entry { uint32_t dir_cluster; uint32_t tail_cluster; };
static dir_tail_entry dir_tail_cache[DIR_TAIL_CACHE_SIZE];

static uint32_t dir_tail_lookup(uint32_t dc) {
    for (int i = 0; i < DIR_TAIL_CACHE_SIZE; i++) {
        if (dir_tail_cache[i].dir_cluster == dc)
            return dir_tail_cache[i].tail_cluster;
    }
    return 0xFFFFFFFF;
}

static void dir_tail_update(uint32_t dc, uint32_t tc) {
    for (int i = 0; i < DIR_TAIL_CACHE_SIZE; i++) {
        if (dir_tail_cache[i].dir_cluster == dc) {
            dir_tail_cache[i].tail_cluster = tc;
            return;
        }
    }
    // Insert new entry
    for (int i = 0; i < DIR_TAIL_CACHE_SIZE; i++) {
        if (dir_tail_cache[i].dir_cluster == 0) {
            dir_tail_cache[i].dir_cluster = dc;
            dir_tail_cache[i].tail_cluster = tc;
            return;
        }
    }
}

// Helper: get pending_op index from pointer
static int pending_op_index(pending_op *op) {
    return (int)(op - pending_pool);
}

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

// Find pending_op by its disk_io cookie (for async completion matching)
static pending_op *find_pending_op_by_cookie(uint32_t cookie) {
    for (int i = 0; i < MAX_PENDING_OPS; i++) {
        if (pending_pool[i].client_pid >= 0 && pending_pool[i].io_active &&
            pending_pool[i].io.cookie == cookie) {
            return &pending_pool[i];
        }
    }
    return NULL;
}

    // Helper: send reply and free pending_op
static void op_complete(pending_op *op, int32_t status, uint32_t count) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = status;
    resp->count = count;
    if (status != 0) {
    }
    // op_complete: send reply and free
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
static bool is_valid_83(const char *name, int name_len);

// Returns cache slot on hit, -1 on miss (disk_io submitted)
static int read_cluster_async(uint32_t cluster, pending_op *op) {
    int slot = cache_lookup(cluster);
    if (slot >= 0) return slot;

    slot = cache_alloc(cluster);
    if (slot < 0) return -1; // all pinned

    uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;
    op->io.lba = lba;
    op->io.count = sectors_per_cluster;
    op->io.buf = cache[slot].data;
    op->io.dir = 0;
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
    op->io.buf = fat_cache[slot].data;
    op->io.dir = 0;
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
// fat_write_entry and write_dir_entry structs are defined above (before pending_op).

// ===================== Async FAT dual-write (sequential) =====================
// Phase 0: read FAT sector into fws->sector_buf (via fat_cache)
// Phase 1: modify entry, write FAT1 via sys_block_async(dir=1)
// Phase 2: write FAT2 via sys_block_async(dir=1)
// Phase 3: update fat_cache from sector_buf, done
//
// The caller must set fws->cluster, fws->value before calling fat_dual_write_start.
// The caller's resume function will be called when phase reaches 3.

static void fat_dual_write_start(fat_write_state *fws, pending_op *op) {
    uint32_t fat_offset = fws->cluster * 4;
    fws->fat_sector = fat_start_lba + (fat_offset / 512);
    fws->fat2_sector = fws->fat_sector + spf32;
    fws->offset_in_sector = fat_offset % 512;
    fws->phase = 0;
    fws->result = 0;

    // Phase 0: read FAT sector into sector_buf
    int slot = fat_cache_lookup(fws->fat_sector);
    if (slot >= 0) {
        // Cache hit — copy to sector_buf and advance
        __memcpy(fws->sector_buf, fat_cache[slot].data, 512);
        fws->fat_cache_slot = slot;
        fws->phase = 1;
    } else {
        // Cache miss — need async read. We read into fat_cache, then copy.
        slot = fat_cache_alloc(fws->fat_sector);
        fws->fat_cache_slot = slot;
        op->io.lba = fws->fat_sector;
        op->io.count = 1;
        op->io.buf = fat_cache[slot].data;
        op->io.dir = 0;
        op->io.complete = [](disk_io *io) {
            pending_op *o = (pending_op *)io->ctx;
            o->io_active = false;
            fat_write_state *f = &o->u.write.fws;
            __memcpy(f->sector_buf, fat_cache[f->fat_cache_slot].data, 512);
            f->phase = 1;
            o->resume(o);
        };
        op->io.ctx = op;
        op->io.next = NULL;
        op->io_active = true;
        submit_disk_io(&op->io);
        return;
    }
    // If we got here with phase=1, proceed to write FAT1
    // Modify the FAT entry in sector_buf
    uint8_t *p = fws->sector_buf + fws->offset_in_sector;
    uint32_t old = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
    uint32_t nv = (old & 0xF0000000) | (fws->value & 0x0FFFFFFF);
    p[0]=nv&0xFF; p[1]=(nv>>8)&0xFF; p[2]=(nv>>16)&0xFF; p[3]=(nv>>24)&0xFF;

    // Phase 1: write FAT1
    op->io.lba = fws->fat_sector;
    op->io.count = 1;
    op->io.buf = fws->sector_buf;
    op->io.dir = 1;
    op->io.complete = [](disk_io *io) {
        pending_op *o = (pending_op *)io->ctx;
        o->io_active = false;
        o->u.write.fws.phase = 2;
        o->resume(o);
    };
    op->io.ctx = op;
    op->io.next = NULL;
    op->io_active = true;
    submit_disk_io(&op->io);
}

// Called when an async FAT write I/O completes. Advances through phases.
// Returns true if FAT dual-write is done (phase=3).
static bool fat_dual_write_resume(fat_write_state *fws, pending_op *op) {
    if (fws->phase == 1) {
        // Phase 0 read completed (cache miss path) — modify entry and write FAT1
        uint8_t *p = fws->sector_buf + fws->offset_in_sector;
        uint32_t old = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
        uint32_t nv = (old & 0xF0000000) | (fws->value & 0x0FFFFFFF);
        p[0]=nv&0xFF; p[1]=(nv>>8)&0xFF; p[2]=(nv>>16)&0xFF; p[3]=(nv>>24)&0xFF;

        op->io.lba = fws->fat_sector;
        op->io.count = 1;
        op->io.buf = fws->sector_buf;
        op->io.dir = 1;
        op->io.complete = [](disk_io *io) {
            pending_op *o = (pending_op *)io->ctx;
            o->io_active = false;
            o->u.write.fws.phase = 2;
            o->resume(o);
        };
        op->io.ctx = op;
        op->io.next = NULL;
        op->io_active = true;
        submit_disk_io(&op->io);
        return false;
    }
    if (fws->phase == 2) {
        // Phase 2: write FAT2
        op->io.lba = fws->fat2_sector;
        op->io.count = 1;
        op->io.buf = fws->sector_buf;
        op->io.dir = 1;
        op->io.complete = [](disk_io *io) {
            pending_op *o = (pending_op *)io->ctx;
            o->io_active = false;
            o->u.write.fws.phase = 3;
            // Update fat_cache from sector_buf
            int slot = o->u.write.fws.fat_cache_slot;
            if (slot >= 0 && slot < FAT_CACHE_PAGES)
                __memcpy(fat_cache[slot].data, o->u.write.fws.sector_buf, 512);
            o->resume(o);
        };
        op->io.ctx = op;
        op->io.next = NULL;
        op->io_active = true;
        submit_disk_io(&op->io);
        return false;
    }
    return (fws->phase == 3);
}

// ===================== Async cluster allocation =====================
// Uses next_free_hint for fast scanning. Scans FAT cache for zero entries.
// On miss, reads additional FAT sectors via fat_cache_read_async.
// Once found, uses fat_dual_write to mark cluster as EOF (0x0FFFFFFF).
// Returns 0 on success, -1 if async I/O submitted (will call resume_write).

static int allocate_cluster_async(pending_op *op, fat_write_state *fws, uint32_t *allocated_out) {
    // Scan FAT cache starting from next_free_hint
    for (uint32_t sector = 0; sector < spf32; sector++) {
        uint32_t abs_sector = ((next_free_hint / 128) + sector) % spf32;
        int slot = fat_cache_lookup(fat_start_lba + abs_sector);
        if (slot < 0) {
            // Need to read this FAT sector — submit async
            // We'll retry on resume
            int cs = fat_cache_alloc(fat_start_lba + abs_sector);
            op->io.lba = fat_start_lba + abs_sector;
            op->io.count = 1;
            op->io.buf = fat_cache[cs].data;
            op->io.dir = 0;
            op->io.complete = [](disk_io *io) {
                pending_op *o = (pending_op *)io->ctx;
                o->io_active = false;
                o->resume(o);  // will re-enter allocate logic
            };
            op->io.ctx = op;
            op->io.next = NULL;
            op->io_active = true;
            submit_disk_io(&op->io);
            return -1;
        }
        uint8_t *fd = fat_cache[slot].data;
        for (int i = 0; i < 128; i++) {
            uint32_t c = abs_sector * 128 + i;
            if (c < 2 || c >= total_data_clusters + 2) continue;
            uint32_t e = (uint32_t)fd[i*4] | ((uint32_t)fd[i*4+1] << 8) |
                          ((uint32_t)fd[i*4+2] << 16) | ((uint32_t)fd[i*4+3] << 24);
            e &= 0x0FFFFFFF;
            if (e == 0) {
                // Found free cluster
                next_free_hint = c + 1;
                if (next_free_hint >= total_data_clusters + 2) next_free_hint = 2;
                *allocated_out = c;
                // Mark as EOF via FAT dual-write
                fws->cluster = c;
                fws->value = 0x0FFFFFFF;
                fat_dual_write_start(fws, op);
                return -1; // async, will resume when done
            }
        }
    }
    // No free cluster found
    return 0; // returns 0 with *allocated_out unchanged to indicate ENOSPC
}

// ===================== Async zero-fill cluster =====================
// Zeros the cluster_buf and writes it to disk via async write.

static void zero_fill_cluster_async(pending_op *op, uint32_t cluster, void *buf) {
    __memset(buf, 0, bytes_per_cluster);
    uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;
    op->io.lba = lba;
    op->io.count = sectors_per_cluster;
    op->io.buf = buf;
    op->io.dir = 1;
    op->io.complete = [](disk_io *io) {
        pending_op *o = (pending_op *)io->ctx;
        o->io_active = false;
        o->resume(o);
    };
    op->io.ctx = op;
    op->io.next = NULL;
    op->io_active = true;
    submit_disk_io(&op->io);
}

// ===================== Async dir entry update =====================
// Uses write_dir_state sub-machine.
// Phase 0: read cluster (async if cache miss)
// Phase 1: modify entry in cluster data, write affected sector(s)
// Phase 2: write second sector if cross-boundary
// Phase 3: done

static void write_dir_entry_async(pending_op *op, write_dir_state *wds,
                                   uint32_t cluster, int index, const fat_dir_entry *entry) {
    wds->cluster = cluster;
    wds->index = index;
    wds->entry = *entry;
    wds->phase = 0;
    wds->need_second_sector = 0;
    wds->result = 0;

    // Phase 0: read cluster
    int slot = cache_lookup(cluster);
    if (slot < 0) {
        // Cache miss — need async read
        slot = cache_alloc(cluster);
        op->io.lba = data_start_lba + (cluster - 2) * sectors_per_cluster;
        op->io.count = sectors_per_cluster;
        op->io.buf = cache[slot].data;
        op->io.dir = 0;
        op->io.complete = [](disk_io *io) {
            pending_op *o = (pending_op *)io->ctx;
            o->io_active = false;
            o->resume(o);  // re-enter, cache now populated
        };
        op->io.ctx = op;
        op->io.next = NULL;
        op->io_active = true;
        submit_disk_io(&op->io);
        return;
    }

    // Cache hit — modify entry directly
    wds->cache_slot = slot;
    __memcpy(cache[slot].data + index * 32, entry, 32);

    // Calculate sector(s) to write back
    int eo = index * 32;
    wds->sector_in_cluster = eo / 512;
    uint32_t clba = data_start_lba + (cluster - 2) * sectors_per_cluster;

    // Check if entry spans sector boundary
    int ois = eo % 512;
    if (ois + 32 > 512 && wds->sector_in_cluster + 1 < (int)sectors_per_cluster)
        wds->need_second_sector = 1;

    wds->phase = 1;

    // Write first sector
    op->io.lba = clba + wds->sector_in_cluster;
    op->io.count = 1;
    op->io.buf = cache[slot].data + wds->sector_in_cluster * 512;
    op->io.dir = 1;
    op->io.complete = [](disk_io *io) {
        pending_op *o = (pending_op *)io->ctx;
        o->io_active = false;
        o->resume(o);
    };
    op->io.ctx = op;
    op->io.next = NULL;
    op->io_active = true;
    submit_disk_io(&op->io);
}

// Continue dir entry write (called after sector write completion)
// Phase 1: first sector write completed — check if second sector needed
// Phase 2: second sector write needed — submit it
// Phase 3: done
static void write_dir_entry_resume(pending_op *op, write_dir_state *wds) {
    if (wds->phase == 1) {
        // First sector write completed
        if (wds->need_second_sector) {
            wds->phase = 2;
            uint32_t clba = data_start_lba + (wds->cluster - 2) * sectors_per_cluster;
            op->io.lba = clba + wds->sector_in_cluster + 1;
            op->io.count = 1;
            op->io.buf = cache[wds->cache_slot].data + (wds->sector_in_cluster + 1) * 512;
            op->io.dir = 1;
            op->io.complete = [](disk_io *io) {
                pending_op *o = (pending_op *)io->ctx;
                o->io_active = false;
                o->resume(o);
            };
            op->io.ctx = op;
            op->io.next = NULL;
            op->io_active = true;
            submit_disk_io(&op->io);
        } else {
            wds->phase = 3;
        }
    } else if (wds->phase == 2) {
        // Second sector write completed
        wds->phase = 3;
    }
}

// ===================== Multi-client session management =====================
#define MAX_CLIENTS  16
#define MAX_SESSION_FDS 8

struct session_open_file {
    bool     used;
    uint32_t start_cluster;
    uint32_t file_size;
    uint64_t offset;              // current file offset (read/write)
    uint32_t dir_start_cluster;   // directory cluster containing file entry
    int      dir_entry_index;     // short-name entry index in that cluster
    bool     dir_entry_valid;     // true if dir cache populated
    uint32_t flags;               // open flags (O_WRONLY, O_RDWR, O_APPEND)
    bool     is_dir_session;      // true for opendir sessions
    // Directory scan state (for opendir/readdir sessions)
    uint32_t dir_cur_cluster;     // current cluster during dir scan
    int      dir_entry_idx;       // current entry index during scan
    char     dir_entry_lfn[256];  // LFN accumulation buffer during dir scan
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
            for (int j = 0; j < MAX_SESSION_FDS; j++) {
                sessions[i].open_files[j].used = false;
                sessions[i].open_files[j].offset = 0;
                sessions[i].open_files[j].dir_start_cluster = 0;
                sessions[i].open_files[j].dir_entry_index = 0;
                sessions[i].open_files[j].dir_entry_valid = false;
                sessions[i].open_files[j].flags = 0;
                sessions[i].open_files[j].is_dir_session = false;
                sessions[i].open_files[j].dir_cur_cluster = 0;
                sessions[i].open_files[j].dir_entry_idx = 0;
                sessions[i].open_files[j].dir_entry_lfn[0] = 0;
            }
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
    readahead_io.buf = cache[slot].data;
    readahead_io.dir = 0;
    readahead_io.complete = readahead_complete;
    readahead_io.ctx = NULL;
    readahead_io.cookie = 0;  // will be set by submit_disk_io
    readahead_io.next = NULL;
    submit_disk_io(&readahead_io);
}

static void readahead_complete(disk_io *io) {
    readahead_pending = false;
    // Data already copied to cache slot by submit_disk_io
}

// ===================== resolve_step (reusable async path resolution) =====================
// Drives resolve_state through RS_INIT..RS_DONE phases.
// Returns RESOLVE_DONE, RESOLVE_ASYNC, or RESOLVE_ERROR.
// Caller re-enters on async I/O completion by calling resolve_step again.

static int resolve_step(resolve_state *rs, pending_op *op) {
    while (1) {
        switch (rs->phase) {

        case RS_INIT: {
            // Extract path component(s)
            if (rs->is_parent) {
                // Find last slash to separate parent path from leaf name
                int last_slash = -1;
                int path_len = 0;
                while (rs->path[path_len]) {
                    if (rs->path[path_len] == '/') last_slash = path_len;
                    path_len++;
                }
                rs->is_parent_end = last_slash;  // parent path ends here
                if (path_len == 1) {
                    // Path is just "/" — no parent to resolve
                    rs->found = false;
                    rs->phase = RS_DONE;
                    return RESOLVE_DONE;
                }
                // Extract leaf name after last slash
                const char *ls = rs->path + last_slash + 1;
                rs->leaf_len = 0;
                while (ls[rs->leaf_len] && rs->leaf_len < 255) {
                    rs->leaf_name[rs->leaf_len] = ls[rs->leaf_len];
                    rs->leaf_len++;
                }
                rs->leaf_name[rs->leaf_len] = '\0';

                // If last_slash == 0, parent is root
                if (last_slash == 0) {
                    rs->dir_cluster = root_cluster;
                    rs->current_cluster = root_cluster;
                    rs->result_cluster = root_cluster;
                    rs->found = true;
                    rs->phase = RS_DONE;
                    return RESOLVE_DONE;
                }
                // Parent path: path[0..last_slash-1]
                rs->path_pos = 1; // start after leading '/'
                rs->comp_start = 1;
                rs->comp_len = 0;
                const char *p = rs->path + 1;
                while (p < rs->path + last_slash && *p && *p != '/') {
                    rs->comp_len++;
                    p++;
                }
                // Check if parent path is just "/" (i.e., "/" + single component)
                if (rs->path[1] == '\0' || (rs->comp_start + rs->comp_len >= last_slash)) {
                    // Only one component in parent path
                    // If that component plus '/' is the entire parent path
                    int next_pos = rs->comp_start + rs->comp_len;
                    if (rs->path[next_pos] == '/' || next_pos == last_slash) {
                        // This is the only component — resolve it in root dir
                        rs->dir_cluster = root_cluster;
                        rs->current_cluster = root_cluster;
                        rs->entry_idx = 0;
                        rs->lfn_buf[0] = '\0';
                        rs->phase = RS_SCAN_ENTRIES;
                        continue;
                    }
                }
                rs->dir_cluster = root_cluster;
                rs->current_cluster = root_cluster;
                rs->entry_idx = 0;
                rs->lfn_buf[0] = '\0';
                rs->phase = RS_SCAN_ENTRIES;
                continue;
            }

            // is_parent=false: full path resolution
            if (rs->path[0] != '/') {
                rs->found = false;
                rs->phase = RS_DONE;
                return RESOLVE_ERROR;
            }
            if (rs->path[1] == '\0') {
                // Root directory
                rs->result.attr = 0x10;
                rs->result.fst_clus_hi = (root_cluster >> 16) & 0xFFFF;
                rs->result.fst_clus_lo = root_cluster & 0xFFFF;
                rs->result.file_size = 0;
                rs->result_cluster = root_cluster;
                rs->result_entry_idx = -1;
                rs->found = true;
                rs->phase = RS_DONE;
                return RESOLVE_DONE;
            }
            // Parse first component
            rs->path_pos = 1;
            rs->comp_start = 1;
            rs->comp_len = 0;
            const char *p2 = rs->path + 1;
            while (*p2 && *p2 != '/') { rs->comp_len++; p2++; }
            rs->dir_cluster = root_cluster;
            rs->current_cluster = root_cluster;
            rs->entry_idx = 0;
            rs->lfn_buf[0] = '\0';
            rs->phase = RS_SCAN_ENTRIES;
            continue;
        }

        case RS_READ_CLUSTER: {
            // Async read dir cluster
            int slot = read_cluster_async(rs->current_cluster, op);
            if (slot < 0) {
                rs->phase = RS_SCAN_ENTRIES; // will scan on resume
                return RESOLVE_ASYNC;
            }
            // Cache hit — go straight to scan
            rs->phase = RS_SCAN_ENTRIES;
            continue;
        }

        case RS_SCAN_ENTRIES: {
            // Scan entries in current cluster
            int slot = cache_lookup(rs->current_cluster);
            if (slot < 0) {
                // Cache miss — need to read
                rs->phase = RS_READ_CLUSTER;
                continue;
            }
            uint8_t *data = cache[slot].data;
            int entries = bytes_per_cluster / 32;
            int comp_start = rs->comp_start;
            int comp_len = rs->comp_len;

            for (; rs->entry_idx < entries; rs->entry_idx++) {
                fat_dir_entry *de = (fat_dir_entry *)(data + rs->entry_idx * 32);
                if (de->name[0] == 0x00) {
                    // End of directory — not found
                    rs->found = false;
                    rs->phase = RS_DONE;
                    return RESOLVE_DONE;
                }
                if (de->name[0] == 0xE5) {
                    rs->lfn_buf[0] = '\0';
                    continue;
                }
                if (de->attr == 0x0F) {
                    collect_lfn_entry(de, rs->lfn_buf);
                    continue;
                }

                // Short name entry — check match
                bool matched = false;
                if (rs->lfn_buf[0] != '\0')
                    matched = match_lfn_name(rs->lfn_buf, rs->path + comp_start, comp_len);
                if (!matched)
                    matched = match_83_name(de->name, rs->path + comp_start, comp_len);

                rs->lfn_buf[0] = '\0';

                if (matched) {
                    rs->result = *de;
                    rs->result_cluster = rs->current_cluster;
                    rs->result_entry_idx = rs->entry_idx;

                    // Advance to next component
                    int next_start = comp_start + comp_len;
                    if (rs->path[next_start] == '/') next_start++;
                    rs->comp_start = next_start;
                    rs->comp_len = 0;
                    const char *np = rs->path + next_start;
                    while (*np && *np != '/') { np++; rs->comp_len++; }
                    rs->entry_idx = 0;

                    if (rs->comp_len > 0) {
                        // More components — descend into directory
                        if (!(de->attr & 0x10)) {
                            rs->found = false;
                            rs->phase = RS_DONE;
                            return RESOLVE_ERROR;
                        }
                        uint32_t next_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
                        if (next_cluster == 0) next_cluster = root_cluster;

                        // In is_parent mode: check if the next component is the
                        // leaf name (beyond the parent path). If so, we've
                        // resolved the parent directory — return immediately.
                        if (rs->is_parent && rs->comp_start >= rs->is_parent_end) {
                            rs->result_cluster = next_cluster;
                            rs->found = true;
                            rs->phase = RS_DONE;
                            return RESOLVE_DONE;
                        }

                        rs->dir_cluster = next_cluster;
                        rs->current_cluster = next_cluster;
                        rs->lfn_buf[0] = '\0';
                        rs->entry_idx = 0;
                        rs->phase = RS_SCAN_ENTRIES;
                        // Break out of for-loop, re-enter while(1) to scan the new directory
                        break;
                    }

                    // No more components — found!
                    rs->found = true;
                    rs->phase = RS_DONE;
                    return RESOLVE_DONE;
                }
            }

            // After for-loop: if a match caused a break (entry_idx < entries),
            // the phase was already set by the match handler — just continue.
            // Otherwise, all entries scanned without match — follow FAT chain.
            if (rs->entry_idx >= entries) {
                rs->phase = RS_READ_FAT;
            }
            continue;
        }

        case RS_READ_FAT: {
            // Follow FAT chain to next cluster
            uint32_t next = fat_read_entry_cached(rs->current_cluster);
            if (next == 0xFFFFFFFF) {
                // FAT cache miss — async read
                uint32_t fat_offset = rs->current_cluster * 4;
                uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                int slot = fat_cache_read_async(fat_sector, op);
                if (slot < 0) {
                    // Will retry on resume
                    return RESOLVE_ASYNC;
                }
                // FAT now in cache — retry
                continue;
            }
            if (next >= 0x0FFFFFF8) {
                // End of chain — not found
                rs->found = false;
                rs->phase = RS_DONE;
                return RESOLVE_DONE;
            }
            rs->current_cluster = next;
            rs->entry_idx = 0;
            rs->lfn_buf[0] = '\0';
            rs->phase = RS_SCAN_ENTRIES;
            continue;
        }

        case RS_DONE:
            return RESOLVE_DONE;

        default:
            rs->found = false;
            rs->phase = RS_DONE;
            return RESOLVE_ERROR;
        }
    }
}

// ===================== find_slots_step (async dir slot scanning + chain extend) =====================
// Drives find_slots_state through FS_SCAN_CLUSTER..FS_DONE phases.
// Returns FIND_SLOTS_DONE, FIND_SLOTS_ASYNC, or FIND_SLOTS_ERROR.

static int find_slots_step(find_slots_state *fss, pending_op *op,
                           fat_write_state *fws, uint8_t *cluster_buf) {
    while (1) {
        switch (fss->phase) {

        case FS_SCAN_CLUSTER: {
            // Read cluster and scan entries
            int slot = read_cluster_async(fss->current_cluster, op);
            if (slot < 0) {
                // Cache miss — async read submitted, will re-enter on completion
                fss->phase = FS_SCAN_CLUSTER;
                return FIND_SLOTS_ASYNC;
            }

            // Cache hit — scan entries for free run
            uint8_t *data = cache[slot].data;
            int epc = bytes_per_cluster / 32;

            for (; fss->entry_idx < epc; fss->entry_idx++) {
                fat_dir_entry *de = (fat_dir_entry *)(data + fss->entry_idx * 32);
                if (de->name[0] == 0x00) {
                    // End-of-directory marker — this slot and all subsequent
                    // slots in this cluster are free (FAT32 spec: all entries
                    // after the first 0x00 are also 0x00).
                    if (fss->run_len == 0) fss->run_start = fss->entry_idx;
                    fss->end_of_dir = true;
                    // Count remaining entries in this cluster as free
                    fss->run_len += (epc - fss->entry_idx);
                    fss->entry_idx = epc;  // skip to end of cluster
                    if (fss->run_len >= fss->needed) {
                        for (int j = 0; j < fss->needed; j++) {
                            fss->slots[j].cluster = fss->current_cluster;
                            fss->slots[j].index = fss->run_start + j;
                        }
                        fss->phase = FS_FOUND;
                        return FIND_SLOTS_DONE;
                    }
                    // Not enough even with all remaining — fall through
                } else if (de->name[0] == 0xE5) {
                    // Deleted entry — reusable free slot
                    if (fss->run_len == 0) fss->run_start = fss->entry_idx;
                    fss->run_len++;
                    if (fss->run_len >= fss->needed) {
                        for (int j = 0; j < fss->needed; j++) {
                            fss->slots[j].cluster = fss->current_cluster;
                            fss->slots[j].index = fss->run_start + j;
                        }
                        fss->phase = FS_FOUND;
                        return FIND_SLOTS_DONE;
                    }
                } else {
                    fss->run_len = 0;
                }
            }

            // If past end-of-directory with insufficient contiguous slots,
            // skip directly to extending the directory chain.
            if (fss->end_of_dir && fss->run_len < fss->needed) {
                fss->phase = FS_READ_FAT;  // find chain tail via normal path
                continue;
            }

            // All entries in this cluster scanned — follow FAT chain
            fss->phase = FS_READ_FAT;
            continue;
        }

        case FS_READ_FAT: {
            uint32_t next = fat_read_entry_cached(fss->current_cluster);
            if (next == 0xFFFFFFFF) {
                // FAT cache miss — async read
                uint32_t fat_offset = fss->current_cluster * 4;
                uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                if (fat_cache_read_async(fat_sector, op) < 0) {
                    // Will retry on resume
                    return FIND_SLOTS_ASYNC;
                }
                // Cache now populated — retry
                continue;
            }
            if (next >= 0x0FFFFFF8) {
                // End of chain — need to extend
                fss->phase = FS_EXTEND_ALLOC;
                continue;
            }
            // Advance to next cluster
            fss->current_cluster = next;
            fss->entry_idx = 0;
            fss->run_len = 0;
            fss->end_of_dir = false;  // reset for new cluster scan
            fss->phase = FS_SCAN_CLUSTER;
            continue;
        }

        case FS_EXTEND_ALLOC: {
            // Allocate a new cluster
            int ret = allocate_cluster_async(op, fws, &fss->new_cluster);
            if (ret == 0) {
                // No free cluster found (ENOSPC)
                fss->phase = FS_DONE;
                return FIND_SLOTS_ERROR;
            }
            // allocate_cluster_async submitted async I/O or started FAT dual-write
            // We'll check fws completion in EXTEND_ZERO
            fss->phase = FS_EXTEND_ZERO;
            return FIND_SLOTS_ASYNC;
        }

        case FS_EXTEND_ZERO: {
            // Wait for FAT dual-write completion from allocate_cluster_async
            if (fws->phase > 0 && fws->phase < 3) {
                bool done = fat_dual_write_resume(fws, op);
                if (!done) return FIND_SLOTS_ASYNC;
            }
            // FAT dual-write done — new cluster is allocated and marked as EOF
            // Zero-fill and write to disk
            __memset(cluster_buf, 0, bytes_per_cluster);
            uint32_t lba = data_start_lba + (fss->new_cluster - 2) * sectors_per_cluster;
            op->io.lba = lba;
            op->io.count = sectors_per_cluster;
            op->io.buf = cluster_buf;
            op->io.dir = 1;
            op->io.complete = [](disk_io *io) {
                pending_op *o = (pending_op *)io->ctx;
                o->io_active = false;
                o->resume(o);
            };
            op->io.ctx = op;
            op->io.next = NULL;
            op->io_active = true;
            submit_disk_io(&op->io);
            fss->phase = FS_EXTEND_LINK;
            return FIND_SLOTS_ASYNC;
        }

        case FS_EXTEND_LINK: {
            // Link new cluster into the directory chain
            // Find tail of directory chain
            if (!fss->tail_traversing) {
                // First: try dir_tail_lookup
                uint32_t tail = dir_tail_lookup(fss->dir_cluster);
                if (tail != 0xFFFFFFFF) {
                    fss->tail_cluster = tail;
                } else {
                    // Cache miss — traverse chain manually
                    fss->tail_traversing = true;
                    fss->traverse_cluster = fss->dir_cluster;
                    fss->phase = FS_EXTEND_LINK; // stay in this phase
                    // Start traversal
                    uint32_t next = fat_read_entry_cached(fss->traverse_cluster);
                    if (next == 0xFFFFFFFF) {
                        // FAT miss — read FAT sector
                        uint32_t fat_offset = fss->traverse_cluster * 4;
                        uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                        if (fat_cache_read_async(fat_sector, op) < 0)
                            return FIND_SLOTS_ASYNC;
                        continue;
                    }
                    // Traverse — keep going until end of chain
                    fss->tail_cluster = fss->traverse_cluster;
                    while (next >= 2 && next < 0x0FFFFFF8) {
                        fss->tail_cluster = next;
                        next = fat_read_entry_cached(next);
                        if (next == 0xFFFFFFFF) {
                            // FAT miss during traversal
                            fss->traverse_cluster = fss->tail_cluster;
                            uint32_t fat_offset2 = fss->traverse_cluster * 4;
                            uint32_t fat_sector2 = fat_start_lba + (fat_offset2 / 512);
                            if (fat_cache_read_async(fat_sector2, op) < 0)
                                return FIND_SLOTS_ASYNC;
                            // Will continue traversal on resume
                            continue;
                        }
                    }
                    // Found tail — update cache
                    dir_tail_update(fss->dir_cluster, fss->tail_cluster);
                    fss->tail_traversing = false;
                }
            }

            // We have tail_cluster — link tail -> new_cluster via FAT dual-write
            fws->phase = 0;
            fws->cluster = fss->tail_cluster;
            fws->value = fss->new_cluster;
            fat_dual_write_start(fws, op);
            // Will check fws completion on next resume
            fss->phase = FS_EXTEND_LINK_WAIT; // internal sub-phase
            return FIND_SLOTS_ASYNC;
        }

        // Internal sub-phase: waiting for FAT dual-write completion of link
        case FS_EXTEND_LINK_WAIT: {
            if (fws->phase > 0 && fws->phase < 3) {
                bool done = fat_dual_write_resume(fws, op);
                if (!done) return FIND_SLOTS_ASYNC;
            }
            // FAT dual-write done — chain linked
            // Update caches
            dir_tail_update(fss->dir_cluster, fss->new_cluster);
            cache_invalidate(fss->dir_cluster);

            // Slots are all in the new cluster (it's empty)
            for (int j = 0; j < fss->needed; j++) {
                fss->slots[j].cluster = fss->new_cluster;
                fss->slots[j].index = j;
            }
            fss->end_of_dir = true;  // new cluster is past end-of-directory
            fss->phase = FS_FOUND;
            return FIND_SLOTS_DONE;
        }

        case FS_FOUND:
            return FIND_SLOTS_DONE;

        case FS_DONE:
            return FIND_SLOTS_ERROR;

        default:
            fss->phase = FS_DONE;
            return FIND_SLOTS_ERROR;
        }
    }
}

// ===================== Async handlers =====================

// Forward declarations
static void resume_read(pending_op *op);
static void resume_open(pending_op *op);
static void resume_readdir(pending_op *op);
static void resume_raw_read(pending_op *op);
static void resume_create_dir(pending_op *op);
static void resume_write(pending_op *op);

// ---- READ ----
static void start_read(pending_op *op, struct client_session *sess,
                        uint32_t fs_fd, uint32_t count) {
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
    uint64_t offset = f->offset;

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

        // Compute write offset from cluster position (not from bytes_read)
        struct client_session *sess2 = &sessions[op->session_idx];
        session_open_file *f2 = &sess2->open_files[op->u.read.fs_fd];
        uint32_t cluster_idx = op->u.read.current_cluster - f2->start_cluster;
        uint32_t file_off = op->u.read.offset + cluster_idx * bytes_per_cluster
                           + op->u.read.in_cluster_offset;

        __memcpy(resp->data + file_off,
                 cache[slot].data + op->u.read.in_cluster_offset, to_copy);
        op->u.read.bytes_read = file_off + to_copy;
        op->u.read.in_cluster_offset = 0;

        // Advance to next cluster via FAT cache
        uint32_t next = fat_read_entry_cached(op->u.read.current_cluster);
        if (next == 0xFFFFFFFF) {
            uint32_t fat_offset = op->u.read.current_cluster * 4;
            uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
            if (fat_cache_read_async(fat_sector, op) < 0) return;
            continue;
        }
        if (next >= 0x0FFFFFF8) {
            // End of chain — all data read
            break;
        }
        op->u.read.current_cluster = next;
    }

    // Done: all data clusters read
    resp->status = 0;
    resp->count = op->u.read.bytes_read;

    // Update session offset
    struct client_session *sess = &sessions[op->session_idx];
    if (op->u.read.fs_fd < MAX_SESSION_FDS && sess->open_files[op->u.read.fs_fd].used)
        sess->open_files[op->u.read.fs_fd].offset += op->u.read.bytes_read;

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

    int pi = 0;
    for (; pi < 255 && path[pi]; pi++) op->u.open.path[pi] = path[pi];
    op->u.open.path[pi] = '\0';

    op->u.open.flags = flags;

    // Initialize resolve_state for full path resolution
    resolve_state *rs = &op->u.open.rs;
    rs->path = op->u.open.path;
    rs->path_pos = 0;
    rs->comp_start = 0;
    rs->comp_len = 0;
    rs->dir_cluster = root_cluster;
    rs->current_cluster = root_cluster;
    rs->entry_idx = 0;
    rs->lfn_buf[0] = '\0';
    rs->found = false;
    rs->is_parent = false;
    rs->leaf_len = 0;
    rs->phase = RS_INIT;

    op->resume = resume_open;
    resume_open(op);
}

static void resume_open(pending_op *op) {
    file_resp *resp = (file_resp *)op->reply_buf;
    // Select resolve_state based on op type (different union members)
    resolve_state *rs;
    if (op->type == OP_READDIR) {
        rs = &op->u.readdir.rs;
    } else {
        rs = &op->u.open.rs;
    }
    struct client_session *sess = &sessions[op->session_idx];

    int r = resolve_step(rs, op);
    if (r == RESOLVE_ASYNC) return; // I/O pending

    if (r == RESOLVE_ERROR || !rs->found) {
        op_complete(op, rs->found ? -ENOTDIR : -ENOENT, 0);
        return;
    }

    // Path resolved — rs->result has the dir entry, rs->result_cluster has the cluster
    fat_dir_entry &de = rs->result;

    // Special case: root directory (result_entry_idx == -1)
    if (rs->result_entry_idx < 0) {
        if (op->type == OP_OPEN) {
            op_complete(op, -EISDIR, 0);
            return;
        }
        if (op->type == OP_STAT) {
            resp->status = 0;
            resp->file_size = 0;
            op_complete(op, 0, 0);
            return;
        }
        // READDIR of root — set up readdir with root_cluster
        if (op->type == OP_READDIR) {
            op->u.readdir.dir_cluster = root_cluster;
            op->u.readdir.current_cluster = root_cluster;
            op->u.readdir.lfn_buf[0] = '\0';
            op->u.readdir.entry_count = 0;
            op->u.readdir.out_count = 0;
            op->u.readdir.entry_idx = 0;
            op->resume = resume_readdir;
            resume_readdir(op);
            return;
        }
        op_complete(op, -EINVAL, 0);
        return;
    }

    // READDIR path resolution done — switch to readdir
    if (op->type == OP_READDIR) {
        if (!(de.attr & 0x10)) {
            op_complete(op, -ENOTDIR, 0);
            return;
        }
        uint32_t dir_cluster = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
        if (dir_cluster == 0) dir_cluster = root_cluster;
        op->u.readdir.dir_cluster = dir_cluster;
        op->u.readdir.current_cluster = dir_cluster;
        op->u.readdir.lfn_buf[0] = '\0';
        op->u.readdir.entry_count = 0;
        op->u.readdir.out_count = 0;
        op->u.readdir.entry_idx = 0;
        op->resume = resume_readdir;
        resume_readdir(op);
        return;
    }

    if (op->type == OP_OPEN) {
        if (de.attr & 0x10) {
            op_complete(op, -EISDIR, 0);
            return;
        }
        int fd = session_alloc_fd(sess);
        if (fd < 0) { op_complete(op, -EMFILE, 0); return; }
        uint32_t cluster = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
        sess->open_files[fd].used = true;
        sess->open_files[fd].start_cluster = cluster;
        sess->open_files[fd].file_size = de.file_size;
        sess->open_files[fd].offset = 0;
        sess->open_files[fd].dir_start_cluster = rs->result_cluster;
        sess->open_files[fd].dir_entry_index = rs->result_entry_idx;
        sess->open_files[fd].dir_entry_valid = true;
        sess->open_files[fd].flags = op->u.open.flags;
        sess->open_files[fd].ra_prev_offset = 0;
        sess->open_files[fd].ra_prev_count = 0;
        sess->open_files[fd].ra_sequential = false;

        // Populate dir_tail_cache
        if (cluster >= 2 && cluster < 0x0FFFFFF8) {
            uint32_t tail = cluster;
            uint32_t next = fat_read_entry_cached(tail);
            while (next >= 2 && next < 0x0FFFFFF8) {
                tail = next;
                next = fat_read_entry_cached(tail);
            }
            dir_tail_update(cluster, tail);
        }

        resp->status = 0;
        resp->fd = (uint32_t)fd;
        resp->file_size = de.file_size;
        op_complete(op, 0, 0);
    } else {
        // STAT
        resp->status = 0;
        resp->file_size = de.file_size;
        op_complete(op, 0, 0);
    }
}

// ---- STAT ----
// Reuses open logic
static void start_stat(pending_op *op, const char *path) {
    op->type = OP_STAT;
    start_open(op, NULL, path, 0);
}

// ---- READDIR ----
static void start_readdir(pending_op *op, const char *path,
                            uint32_t rd_offset, uint32_t rd_count) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = 0; resp->fd = 0; resp->file_size = 0; resp->count = 0; resp->total = 0;

    // Copy path to readdir context
    { int ri = 0;
      for (; ri < 255 && path[ri]; ri++) op->u.readdir.path[ri] = path[ri];
      op->u.readdir.path[ri] = '\0';
    }
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

    // Resolve path using resolve_state
    resolve_state *rs = &op->u.readdir.rs;
    rs->path = op->u.readdir.path;
    rs->path_pos = 0;
    rs->comp_start = 0;
    rs->comp_len = 0;
    rs->dir_cluster = root_cluster;
    rs->current_cluster = root_cluster;
    rs->entry_idx = 0;
    rs->lfn_buf[0] = '\0';
    rs->found = false;
    rs->is_parent = false;
    rs->leaf_len = 0;
    rs->phase = RS_INIT;

    // Use resume_open for path resolution, then switch to readdir
    // Set type to OP_READDIR so resume_open dispatches to readdir on completion
    op->resume = resume_open;
    resume_open(op);
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
    op->io.buf = resp->data;
    op->io.dir = 0;
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

// ---- WRITE ----
// Async write state machine. Phases:
//   WPHASE_ACQUIRE_LOCK  - acquire write_lock (may queue)
//   WPHASE_HOLE_ALLOC    - allocate clusters for hole (offset > file_size)
//   WPHASE_HOLE_ZERO     - zero-fill hole clusters
//   WPHASE_HOLE_LINK     - link hole clusters into FAT chain
//   WPHASE_LOCATE        - FAT chain walk to target cluster
//   WPHASE_EXTEND_ALLOC  - allocate new cluster for extend
//   WPHASE_EXTEND_ZERO   - zero-fill new cluster
//   WPHASE_EXTEND_LINK   - link new cluster into chain
//   WPHASE_WRITE_DATA    - write file data to cluster(s)
//   WPHASE_UPDATE_DIR    - update directory entry
//   WPHASE_DONE          - release lock, reply, free

static void start_write(pending_op *op, struct client_session *sess,
                         uint32_t fs_fd, uint32_t count,
                         uint8_t *write_data, size_t data_len) {
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

    // Check write permission (O_WRONLY or O_RDWR)
    uint32_t open_flags = f->flags;
    if (!(open_flags & (O_WRONLY | O_RDWR))) {
        op_complete(op, -EBADF, 0);
        return;
    }

    // Copy write data to pool
    int idx = pending_op_index(op);
    if (data_len > 0 && data_len <= 65536) {
        __memcpy(write_data_bufs[idx], write_data, data_len);
    }
    op->u.write.write_data_len = (uint32_t)data_len;
    op->u.write.pool_idx = idx;
    op->u.write.fs_fd = fs_fd;
    op->u.write.flags = open_flags;
    op->u.write.count = (uint32_t)data_len;
    op->u.write.bytes_written = 0;
    op->u.write.orig_file_size = (uint32_t)f->file_size;
    op->u.write.allocated_cluster = 0;
    op->u.write.tail_cluster = 0;
    op->u.write.chain_pos = 0;
    op->u.write.wds.cluster = 0;  // marks wds as not started
    op->u.write.fws.phase = 0;    // reset fws (union may have stale data from CREATE)
    op->u.write.link_started = false;

    // O_APPEND: set offset to file_size
    if (open_flags & O_APPEND) {
        op->u.write.offset = f->file_size;
    } else {
        op->u.write.offset = f->offset;
    }

    // Clamp count to available data
    if (op->u.write.count > (uint32_t)data_len)
        op->u.write.count = (uint32_t)data_len;

    if (op->u.write.count == 0) {
        // Zero-length write — succeed immediately
        resp->status = 0;
        resp->count = 0;
        resp->file_size = f->file_size;
        op_complete(op, 0, 0);
        return;
    }

    // Calculate new file size
    uint64_t write_end = op->u.write.offset + op->u.write.count;
    op->u.write.new_file_size = (uint32_t)((write_end > f->file_size) ? write_end : f->file_size);

    op->resume = resume_write;

    // Acquire write_lock
    op->u.write.phase = WPHASE_ACQUIRE_LOCK;
    if (!write_lock_acquire(op)) {
        // Queued — set phase so resume_write proceeds correctly when lock is granted
        op->u.write.phase = WPHASE_LOCATE;
        op->u.write.link_started = false;
        return;
    }

    // Lock acquired — start FAT chain walk (handles holes via extend)
    op->u.write.phase = WPHASE_LOCATE;
    op->u.write.link_started = false;

    resume_write(op);
}

static void resume_write(pending_op *op) {
    file_resp *resp = (file_resp *)op->reply_buf;
    struct client_session *sess = &sessions[op->session_idx];
    session_open_file *f = &sess->open_files[op->u.write.fs_fd];

    switch (op->u.write.phase) {

    case WPHASE_LOCATE: {
        // Walk FAT chain from start_cluster to reach the cluster containing offset
        if (op->u.write.chain_pos == 0) {
            op->u.write.offset_clusters = (uint32_t)(op->u.write.offset / bytes_per_cluster);
            op->u.write.in_cluster_offset = (uint32_t)(op->u.write.offset % bytes_per_cluster);

            // If file has no data clusters, need to allocate
            if (f->file_size == 0) {
                op->u.write.phase = WPHASE_EXTEND_ALLOC;
                op->u.write.tail_cluster = 0;
                op->u.write.link_started = false;
                resume_write(op);
                return;
            }

            op->u.write.current_cluster = f->start_cluster;
        }

        // Walk the chain
        while (op->u.write.chain_pos < op->u.write.offset_clusters) {
            uint32_t next = fat_read_entry_cached(op->u.write.current_cluster);
            if (next == 0xFFFFFFFF) {
                // FAT cache miss
                uint32_t fat_offset = op->u.write.current_cluster * 4;
                uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                if (fat_cache_read_async(fat_sector, op) < 0)
                    return; // async I/O submitted
                continue;
            }
            if (next >= 0x0FFFFFF8) {
                // End of chain before target — need to extend
                op->u.write.tail_cluster = op->u.write.current_cluster;
                op->u.write.link_started = false;
                op->u.write.phase = WPHASE_EXTEND_ALLOC;
                resume_write(op);
                return;
            }
            op->u.write.current_cluster = next;
            op->u.write.chain_pos++;
        }

        // Reached target cluster
        op->u.write.phase = WPHASE_WRITE_DATA;
        resume_write(op);
        return;
    }

    case WPHASE_EXTEND_ALLOC: {
        int ret = allocate_cluster_async(op, &op->u.write.fws, &op->u.write.allocated_cluster);
        if (ret == 0) {
            op->u.write.phase = WPHASE_DONE;
            resp->status = -ENOSPC;
            goto write_done;
        }
        // allocate_cluster_async returned -1 (async I/O submitted).
        // If allocated_cluster is set, a free cluster was found and fat_dual_write
        // has been started — advance to EXTEND_ZERO to await its completion.
        // Otherwise, a FAT cache read is in progress and we must stay in
        // WPHASE_EXTEND_ALLOC so the resume re-enters the allocation scan.
        if (op->u.write.allocated_cluster != 0) {
            op->u.write.phase = WPHASE_EXTEND_ZERO;
        }
        return;
    }

    case WPHASE_EXTEND_ZERO: {
        // Check if FAT dual-write for allocation is done
        if (op->u.write.fws.phase != 3) {
            bool done = fat_dual_write_resume(&op->u.write.fws, op);
            if (!done) return;
        }
        if (op->u.write.allocated_cluster == 0) {
            op->u.write.phase = WPHASE_DONE;
            resp->status = -ENOSPC;
            goto write_done;
        }
        zero_fill_cluster_async(op, op->u.write.allocated_cluster, op->u.write.cluster_buf);
        op->u.write.phase = WPHASE_EXTEND_LINK;
        return;
    }

    case WPHASE_EXTEND_LINK: {
        // If FAT dual-write for allocation is in progress, resume it first
        if (op->u.write.fws.phase > 0 && op->u.write.fws.phase < 3) {
            bool done = fat_dual_write_resume(&op->u.write.fws, op);
            if (!done) return; // more async I/O
        }

        uint32_t nc = op->u.write.allocated_cluster;
        if (op->u.write.tail_cluster == 0) {
            // File had no clusters — this is the first cluster
            f->start_cluster = nc;
            op->u.write.current_cluster = nc;
            op->u.write.chain_pos++;
        } else {
            // Link tail -> new cluster via FAT dual-write
            if (!op->u.write.link_started) {
                // Start FAT dual-write to link tail -> nc
                op->u.write.link_started = true;
                op->u.write.fws.phase = 0;  // reset fws for new dual-write
                op->u.write.fws.cluster = op->u.write.tail_cluster;
                op->u.write.fws.value = nc;
                fat_dual_write_start(&op->u.write.fws, op);
                return; // async — will resume on completion
            }
            // Resume FAT dual-write for link if not done
            if (op->u.write.fws.phase < 3) {
                bool done = fat_dual_write_resume(&op->u.write.fws, op);
                if (!done) return;
            }
            // FAT dual-write done
            op->u.write.current_cluster = nc;
            op->u.write.chain_pos++;
        }

        // Check if more clusters needed
        // Calculate total clusters needed for this write
        uint64_t write_end = op->u.write.offset + op->u.write.count;
        uint32_t total_needed = (uint32_t)((write_end + bytes_per_cluster - 1) / bytes_per_cluster);
        if (op->u.write.chain_pos < op->u.write.offset_clusters ||
            (op->u.write.bytes_written < op->u.write.count &&
             op->u.write.chain_pos < total_needed)) {
            // Need more clusters
            op->u.write.tail_cluster = nc;
            op->u.write.link_started = false;
            op->u.write.phase = WPHASE_EXTEND_ALLOC;
            resume_write(op);
            return;
        }

        // Update dir_tail_cache — nc is the new tail of this chain
        if (f->start_cluster >= 2 && f->start_cluster < 0x0FFFFFF8)
            dir_tail_update(f->start_cluster, nc);

        op->u.write.phase = WPHASE_LOCATE;
        resume_write(op);
        return;
    }

    case WPHASE_WRITE_DATA: {
        // Read cluster, modify, write back
        int slot = read_cluster_async(op->u.write.current_cluster, op);
        if (slot < 0) return; // async read submitted

        // Cache hit — modify and write back
        // Pin the cluster to prevent eviction during async write
        cache_pin(slot);

        uint32_t to_copy = bytes_per_cluster - op->u.write.in_cluster_offset;
        if (to_copy > op->u.write.count - op->u.write.bytes_written)
            to_copy = op->u.write.count - op->u.write.bytes_written;

        uint8_t *write_data = write_data_bufs[op->u.write.pool_idx];
        __memcpy(cache[slot].data + op->u.write.in_cluster_offset,
                 write_data + op->u.write.bytes_written, to_copy);

        // Write back affected sector(s)
        int sector_idx = op->u.write.in_cluster_offset / 512;
        uint32_t clba = data_start_lba + (op->u.write.current_cluster - 2) * sectors_per_cluster;

        // Check if write spans sector boundary
        int end_offset = op->u.write.in_cluster_offset + to_copy;
        int end_sector = (end_offset - 1) / 512;

        if (sector_idx == end_sector) {
            // Single sector write
            op->io.lba = clba + sector_idx;
            op->io.count = 1;
            op->io.buf = cache[slot].data + sector_idx * 512;
            op->io.dir = 1;
        } else {
            // Multi-sector write — write from sector_idx to end_sector
            int num_sectors = end_sector - sector_idx + 1;
            if (num_sectors > (int)sectors_per_cluster) num_sectors = sectors_per_cluster - sector_idx;
            op->io.lba = clba + sector_idx;
            op->io.count = num_sectors;
            op->io.buf = cache[slot].data + sector_idx * 512;
            op->io.dir = 1;
        }

        op->io.complete = [](disk_io *io) {
            pending_op *o = (pending_op *)io->ctx;
            // Unpin the cluster now that write is done
            int sl = cache_lookup(o->u.write.current_cluster);
            if (sl >= 0) cache_unpin(sl);
            o->io_active = false;
            o->resume(o);
        };
        op->io.ctx = op;
        op->io.next = NULL;
        op->io_active = true;
        submit_disk_io(&op->io);

        op->u.write.bytes_written += to_copy;
        op->u.write.in_cluster_offset = 0;

        // After async write completes, check if more data to write
        // (handled on next resume)
        op->u.write.phase = WPHASE_WRITE_NEXT;
        return;
    }

    case WPHASE_WRITE_NEXT: {
        if (op->u.write.bytes_written < op->u.write.count) {
            // More data to write — advance to next cluster
            uint32_t next = fat_read_entry_cached(op->u.write.current_cluster);
            if (next == 0xFFFFFFFF) {
                // FAT cache miss
                uint32_t fat_offset = op->u.write.current_cluster * 4;
                uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                if (fat_cache_read_async(fat_sector, op) < 0)
                    return;
                // On resume, we'll be back in this phase — retry
                resume_write(op);
                return;
            }
            if (next >= 0x0FFFFFF8) {
                // End of chain — need to extend
                op->u.write.tail_cluster = op->u.write.current_cluster;
                op->u.write.phase = WPHASE_EXTEND_ALLOC;
                resume_write(op);
                return;
            }
            op->u.write.current_cluster = next;
            op->u.write.chain_pos++;
            op->u.write.in_cluster_offset = 0;
            op->u.write.phase = WPHASE_WRITE_DATA;
            resume_write(op);
            return;
        }
        // All data written — update dir entry
        op->u.write.phase = WPHASE_UPDATE_DIR;
        resume_write(op);
        return;
    }

    case WPHASE_UPDATE_DIR: {
        // Handle dir entry write sub-machine
        // wds.cluster == 0 means we haven't started the dir update yet
        if (op->u.write.wds.cluster == 0 && f->dir_entry_valid) {
            fat_dir_entry updated;
            int slot = cache_lookup(f->dir_start_cluster);
            if (slot >= 0) {
                updated = *(fat_dir_entry *)(cache[slot].data + f->dir_entry_index * 32);
            } else {
                // Need to read dir cluster first
                slot = cache_alloc(f->dir_start_cluster);
                op->io.lba = data_start_lba + (f->dir_start_cluster - 2) * sectors_per_cluster;
                op->io.count = sectors_per_cluster;
                op->io.buf = cache[slot].data;
                op->io.dir = 0;
                op->io.complete = [](disk_io *io) {
                    pending_op *o = (pending_op *)io->ctx;
                    o->io_active = false;
                    o->resume(o); // re-enter WPHASE_UPDATE_DIR, wds.cluster still 0
                };
                op->io.ctx = op;
                op->io.next = NULL;
                op->io_active = true;
                submit_disk_io(&op->io);
                return;
            }

            updated.file_size = op->u.write.new_file_size;
            updated.wrt_date = HARD_DATE;
            updated.wrt_time = HARD_TIME;
            updated.lst_acc_date = HARD_DATE;
            if (op->u.write.orig_file_size == 0) {
                updated.fst_clus_hi = (f->start_cluster >> 16) & 0xFFFF;
                updated.fst_clus_lo = f->start_cluster & 0xFFFF;
            }

            write_dir_entry_async(op, &op->u.write.wds,
                                   f->dir_start_cluster, f->dir_entry_index, &updated);
            return; // async I/O submitted or will complete via wds phases
        }
        // wds.cluster != 0 but wds.phase == 0: write_dir_entry_async had a cache miss
        // and the async read just completed. Re-call to proceed now that cache is populated.
        if (op->u.write.wds.cluster != 0 && op->u.write.wds.phase == 0) {
            write_dir_entry_async(op, &op->u.write.wds,
                                   op->u.write.wds.cluster, op->u.write.wds.index,
                                   &op->u.write.wds.entry);
            return;
        }
        if (op->u.write.wds.phase > 0 && op->u.write.wds.phase < 3) {
            write_dir_entry_resume(op, &op->u.write.wds);
            if (op->u.write.wds.phase < 3) return;
            // Dir entry write done — fall through to phase==3 check
        }
        if (op->u.write.wds.phase == 3) {
            // Dir entry write complete
            op->u.write.phase = WPHASE_DONE;
            resume_write(op);
            return;
        }

        if (!f->dir_entry_valid) {
            // No dir entry cache — skip dir update
            op->u.write.phase = WPHASE_DONE;
            resume_write(op);
            return;
        }
        return;
    }

    case WPHASE_DONE:
    write_done:
        // Update session state
        f->offset = op->u.write.offset + op->u.write.bytes_written;
        f->file_size = op->u.write.new_file_size;

        // Release write_lock
        write_lock_release();

        // Reply
        resp->status = (resp->status != 0) ? resp->status : 0;
        resp->count = op->u.write.bytes_written;
        resp->file_size = op->u.write.new_file_size;
        {
            size_t rlen = sizeof(file_resp);
            // Write completed successfully
            msg_resp(op->reply_buf, rlen);
        }
        free_pending_op(op);
        return;

    default:
        break;
    }
}

// ---- CREATE_DIR (unified create + mkdir async state machine) ----

static void start_create_dir(pending_op *op, const char *path, bool is_mkdir) {
    file_resp *resp = (file_resp *)op->reply_buf;
    resp->status = 0; resp->fd = 0; resp->file_size = 0; resp->count = 0; resp->total = 0;

    if (path[0] != '/') {
        op_complete(op, -ENOENT, 0);
        return;
    }

    create_dir_context *cd = &op->u.create_dir;
    cd->is_mkdir = is_mkdir;

    // Copy path
    int pi = 0;
    for (; pi < 255 && path[pi]; pi++) cd->path[pi] = path[pi];
    cd->path[pi] = '\0';

    // Initialize resolve_state (first: full path resolution)
    resolve_state *rs = &cd->rs;
    rs->path = cd->path;
    rs->path_pos = 0;
    rs->comp_start = 0;
    rs->comp_len = 0;
    rs->dir_cluster = root_cluster;
    rs->current_cluster = root_cluster;
    rs->entry_idx = 0;
    rs->lfn_buf[0] = '\0';
    rs->found = false;
    rs->is_parent = false;
    rs->leaf_len = 0;
    rs->phase = RS_INIT;

    cd->phase = CD_RESOLVE_PATH;
    cd->lfn_written_count = 0;
    cd->allocated_cluster = 0;
    cd->new_dir_cluster = 0;
    cd->wds.phase = 0;
    cd->wds.cluster = 0; // marks wds as not started
    cd->fws.phase = 0;
    cd->gen_short_num = 1;
    cd->gen_short_phase = GS_SCAN;
    cd->gen_short_collision = false;

    op->resume = resume_create_dir;

    // Acquire write_lock (same pattern as write path)
    if (!write_lock_acquire(op)) {
        // Queued — resume_create_dir will be called when lock is released
        return;
    }

    // Lock acquired — start resolve
    resume_create_dir(op);
}

static void resume_create_dir(pending_op *op) {
    file_resp *resp = (file_resp *)op->reply_buf;
    create_dir_context *cd = &op->u.create_dir;

    switch (cd->phase) {

    case CD_RESOLVE_PATH: {
        // Full path resolution to check if target exists
        int r = resolve_step(&cd->rs, op);
        if (r == RESOLVE_ASYNC) return;

        if (r == RESOLVE_ERROR) {
            op_complete(op, -ENOENT, 0);
            return;
        }

        if (cd->rs.found) {
            if (cd->is_mkdir) {
                // mkdir: target must NOT exist
                op_complete(op, -EEXIST, 0);
                return;
            }
            // create: target exists → update timestamp
            // We need to find the entry in parent dir and update it.
            // Reset resolve_state for parent-only resolution
            cd->rs.is_parent = true;
            cd->rs.path = cd->path;
            cd->rs.path_pos = 0;
            cd->rs.comp_start = 0;
            cd->rs.comp_len = 0;
            cd->rs.dir_cluster = root_cluster;
            cd->rs.current_cluster = root_cluster;
            cd->rs.entry_idx = 0;
            cd->rs.lfn_buf[0] = '\0';
            cd->rs.found = false;
            cd->rs.phase = RS_INIT;
            cd->phase = CD_UPDATE_TIMESTAMP;
            resume_create_dir(op);
            return;
        }

        // Not found — resolve parent directory
        cd->rs.is_parent = true;
        cd->rs.path = cd->path;
        cd->rs.path_pos = 0;
        cd->rs.comp_start = 0;
        cd->rs.comp_len = 0;
        cd->rs.dir_cluster = root_cluster;
        cd->rs.current_cluster = root_cluster;
        cd->rs.entry_idx = 0;
        cd->rs.lfn_buf[0] = '\0';
        cd->rs.found = false;
        cd->rs.phase = RS_INIT;
        cd->phase = CD_RESOLVE_PARENT;
        resume_create_dir(op);
        return;
    }

    case CD_RESOLVE_PARENT: {
        int r = resolve_step(&cd->rs, op);
        if (r == RESOLVE_ASYNC) return;

        if (r == RESOLVE_ERROR || !cd->rs.found) {
            op_complete(op, -ENOENT, 0);
            return;
        }

        // Parent directory resolved
        cd->parent_cluster = cd->rs.result_cluster;
        cd->leaf_len = cd->rs.leaf_len;
        // Copy leaf_name from rs to cd
        for (int i = 0; i < cd->leaf_len && i < 255; i++)
            cd->leaf_name_buf[i] = cd->rs.leaf_name[i];
        cd->leaf_name_buf[cd->leaf_len] = '\0';

        // Generate short name (async collision check)
        cd->gen_short_num = 1;
        cd->gen_short_phase = GS_SCAN;
        cd->gen_short_collision = false;
        cd->gen_short_cluster = cd->parent_cluster;
        cd->gen_short_entry_idx = 0;
        cd->gen_short_lfn_buf[0] = '\0';

        // Start gen_short_name async scan
        // We'll call gen_short_name_step inline here
        cd->phase = CD_RESOLVE_PARENT; // stay here until gen_short is done
        // Fall through to gen_short logic below
        goto gen_short_name_step;
    }

gen_short_name_step: {
        // Async short name generation with collision checking
        // For each candidate number, scan parent dir to check collision
        if (cd->gen_short_phase == GS_SCAN) {
            // Build candidate short name
            char candidate[11];
            if (is_valid_83(cd->leaf_name_buf, cd->leaf_len)) {
                format_83_name(cd->leaf_name_buf, cd->leaf_len, candidate);
                // Check collision in parent dir (cache-only first)
                cd->gen_short_cluster = cd->parent_cluster;
                cd->gen_short_entry_idx = 0;
                cd->gen_short_lfn_buf[0] = '\0';
                // Scan entries
                int slot = cache_lookup(cd->gen_short_cluster);
                if (slot < 0) {
                    // Cache miss — async read
                    slot = cache_alloc(cd->gen_short_cluster);
                    op->io.lba = data_start_lba + (cd->gen_short_cluster - 2) * sectors_per_cluster;
                    op->io.count = sectors_per_cluster;
                    op->io.buf = cache[slot].data;
                    op->io.dir = 0;
                    op->io.complete = [](disk_io *io) {
                        pending_op *o = (pending_op *)io->ctx;
                        o->io_active = false;
                        o->resume(o);
                    };
                    op->io.ctx = op;
                    op->io.next = NULL;
                    op->io_active = true;
                    submit_disk_io(&op->io);
                    cd->gen_short_phase = GS_SCAN; // retry on resume
                    return;
                }

                // Scan this cluster
                uint8_t *data = cache[slot].data;
                int entries = bytes_per_cluster / 32;
                bool collision = false;
                char lb[256]; lb[0] = '\0';
                for (int i = 0; i < entries; i++) {
                    fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
                    if (de->name[0] == 0x00) break;
                    if (de->name[0] == 0xE5) { lb[0] = '\0'; continue; }
                    if (de->attr == 0x0F) { collect_lfn_entry(de, lb); continue; }
                    // Check 8.3 name collision
                    bool m = match_83_name(de->name, candidate, 11);
                    lb[0] = '\0';
                    if (m) { collision = true; break; }
                }

                if (!collision) {
                    // No collision — short name is valid
                    for (int i = 0; i < 11; i++) cd->short_name[i] = candidate[i];
                    goto gen_short_done;
                }
                // Collision with valid 8.3 — need tilde name
                goto gen_short_tilde;
            }

gen_short_tilde: {
            // Generate tilde short name candidates
            char basename[9] = {};
            int dot_pos = -1;
            for (int i = cd->leaf_len - 1; i >= 0; i--) {
                if (cd->leaf_name_buf[i] == '.') { dot_pos = i; break; }
            }
            int blen = (dot_pos >= 0) ? dot_pos : cd->leaf_len;
            if (blen > 6) blen = 6;
            for (int i = 0; i < blen; i++) {
                char c = cd->leaf_name_buf[i];
                if (c >= 'a' && c <= 'z') c -= 32;
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
                basename[i] = c;
            }
            char extname[4] = {};
            if (dot_pos >= 0) {
                int elen = cd->leaf_len - dot_pos - 1;
                if (elen > 3) elen = 3;
                for (int i = 0; i < elen; i++) {
                    char c = cd->leaf_name_buf[dot_pos + 1 + i];
                    if (c >= 'a' && c <= 'z') c -= 32;
                    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
                    extname[i] = c;
                }
            }

            // Try candidate numbers starting from gen_short_num
            for (int n = cd->gen_short_num; n <= 99; n++) {
                char cand[11];
                for (int i = 0; i < 11; i++) cand[i] = ' ';
                int tl = (n >= 10) ? 3 : 2;
                int bc = 8 - tl;
                if (bc > 6) bc = 6;
                int pos = 0;
                for (int i = 0; i < bc && basename[i]; i++) cand[pos++] = basename[i];
                cand[pos++] = '~';
                if (n >= 10) { cand[pos++] = '0' + (n / 10); cand[pos++] = '0' + (n % 10); }
                else cand[pos++] = '0' + n;
                for (int i = 0; i < 3 && extname[i]; i++) cand[8 + i] = extname[i];

                // Check collision in parent dir
                cd->gen_short_cluster = cd->parent_cluster;
                cd->gen_short_entry_idx = 0;
                cd->gen_short_lfn_buf[0] = '\0';

                // Scan entries (cache-only)
                int slot2 = cache_lookup(cd->gen_short_cluster);
                if (slot2 < 0) {
                    // Cache miss — store candidate, async read, resume
                    for (int i = 0; i < 11; i++) cd->short_name[i] = cand[i]; // temp store
                    cd->gen_short_num = n;
                    slot2 = cache_alloc(cd->gen_short_cluster);
                    op->io.lba = data_start_lba + (cd->gen_short_cluster - 2) * sectors_per_cluster;
                    op->io.count = sectors_per_cluster;
                    op->io.buf = cache[slot2].data;
                    op->io.dir = 0;
                    op->io.complete = [](disk_io *io) {
                        pending_op *o = (pending_op *)io->ctx;
                        o->io_active = false;
                        o->resume(o);
                    };
                    op->io.ctx = op;
                    op->io.next = NULL;
                    op->io_active = true;
                    submit_disk_io(&op->io);
                    cd->gen_short_phase = GS_READ_CLUSTER;
                    return;
                }

                uint8_t *data2 = cache[slot2].data;
                int entries2 = bytes_per_cluster / 32;
                bool coll = false;
                char lb2[256]; lb2[0] = '\0';
                for (int i = 0; i < entries2; i++) {
                    fat_dir_entry *de = (fat_dir_entry *)(data2 + i * 32);
                    if (de->name[0] == 0x00) break;
                    if (de->name[0] == 0xE5) { lb2[0] = '\0'; continue; }
                    if (de->attr == 0x0F) { collect_lfn_entry(de, lb2); continue; }
                    bool m2 = match_83_name(de->name, cand, 11);
                    lb2[0] = '\0';
                    if (m2) { coll = true; break; }
                }

                if (!coll) {
                    // No collision — use this short name
                    for (int i = 0; i < 11; i++) cd->short_name[i] = cand[i];
                    goto gen_short_done;
                }
                // Collision — try next number
                cd->gen_short_num = n + 1;
            }
            // All candidates exhausted — use UNKNO~1 fallback
            format_83_name("UNKNO~1", 7, cd->short_name);
            goto gen_short_done;
        }

gen_short_done: {
            // Short name generated
            cd->lfn_count = (cd->leaf_len + 12) / 13;
            cd->total_slots = cd->lfn_count + 1;

            if (cd->is_mkdir) {
                cd->phase = CD_ALLOCATE;
            } else {
                cd->phase = CD_FIND_SLOTS;
            }
            resume_create_dir(op);
            return;
        }
        }

        // GS_READ_CLUSTER: came back after async read for collision check
        if (cd->gen_short_phase == GS_READ_CLUSTER) {
            // Re-scan for collision with the stored candidate
            int slot = cache_lookup(cd->gen_short_cluster);
            if (slot < 0) {
                // Should be in cache now after async read — error
                op_complete(op, -EIO, 0);
                return;
            }
            uint8_t *data = cache[slot].data;
            int entries = bytes_per_cluster / 32;
            bool coll = false;
            char lb[256]; lb[0] = '\0';
            for (int i = 0; i < entries; i++) {
                fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
                if (de->name[0] == 0x00) break;
                if (de->name[0] == 0xE5) { lb[0] = '\0'; continue; }
                if (de->attr == 0x0F) { collect_lfn_entry(de, lb); continue; }
                bool m = match_83_name(de->name, cd->short_name, 11);
                lb[0] = '\0';
                if (m) { coll = true; break; }
            }

            if (!coll) {
                // No collision — short name is valid
                goto gen_short_done;
            }
            // Collision — try next candidate
            cd->gen_short_num++;
            cd->gen_short_phase = GS_SCAN;
            goto gen_short_tilde;
        }

        // Shouldn't reach here
        op_complete(op, -EIO, 0);
        return;
    }

    case CD_ALLOCATE: {
        // mkdir only: allocate cluster for new directory
        int ret = allocate_cluster_async(op, &cd->fws, &cd->allocated_cluster);
        if (ret == 0) {
            // ENOSPC
            op_complete(op, -ENOMEM, 0);
            return;
        }
        // FAT dual-write in progress — wait for completion
        cd->phase = CD_ALLOCATE_WAIT;
        return;
    }

    case CD_ALLOCATE_WAIT: {
        // Internal: wait for FAT dual-write completion from allocate
        if (cd->fws.phase > 0 && cd->fws.phase < 3) {
            bool done = fat_dual_write_resume(&cd->fws, op);
            if (!done) return;
        }
        cd->new_dir_cluster = cd->allocated_cluster;
        cd->phase = CD_INIT_DIR;
        resume_create_dir(op);
        return;
    }

    case CD_INIT_DIR: {
        // mkdir only: initialize new directory cluster (./.. entries + write)
        __memset(cd->cluster_buf, 0, bytes_per_cluster);

        // Create . entry pointing to new_dir_cluster
        fat_dir_entry dot_entry;
        dot_entry.name[0] = '.';
        for (int i = 1; i < 11; i++) dot_entry.name[i] = ' ';
        dot_entry.attr = 0x10; dot_entry.nt_res = 0; dot_entry.crt_time_tenth = 0;
        dot_entry.crt_time = HARD_TIME; dot_entry.crt_date = HARD_DATE;
        dot_entry.lst_acc_date = HARD_DATE;
        dot_entry.fst_clus_hi = (cd->new_dir_cluster >> 16) & 0xFFFF;
        dot_entry.wrt_time = HARD_TIME; dot_entry.wrt_date = HARD_DATE;
        dot_entry.fst_clus_lo = cd->new_dir_cluster & 0xFFFF;
        dot_entry.file_size = 0;
        __memcpy(cd->cluster_buf, &dot_entry, 32);

        // Create .. entry pointing to parent_cluster
        fat_dir_entry dotdot_entry;
        dotdot_entry.name[0] = '.'; dotdot_entry.name[1] = '.';
        for (int i = 2; i < 11; i++) dotdot_entry.name[i] = ' ';
        dotdot_entry.attr = 0x10; dotdot_entry.nt_res = 0; dotdot_entry.crt_time_tenth = 0;
        dotdot_entry.crt_time = HARD_TIME; dotdot_entry.crt_date = HARD_DATE;
        dotdot_entry.lst_acc_date = HARD_DATE;
        dotdot_entry.fst_clus_hi = (cd->parent_cluster >> 16) & 0xFFFF;
        dotdot_entry.wrt_time = HARD_TIME; dotdot_entry.wrt_date = HARD_DATE;
        dotdot_entry.fst_clus_lo = cd->parent_cluster & 0xFFFF;
        dotdot_entry.file_size = 0;
        __memcpy(cd->cluster_buf + 32, &dotdot_entry, 32);

        // Async write: entire cluster to disk
        uint32_t lba = data_start_lba + (cd->new_dir_cluster - 2) * sectors_per_cluster;
        op->io.lba = lba;
        op->io.count = sectors_per_cluster;
        op->io.buf = cd->cluster_buf;
        op->io.dir = 1;
        op->io.complete = [](disk_io *io) {
            pending_op *o = (pending_op *)io->ctx;
            o->io_active = false;
            o->resume(o);
        };
        op->io.ctx = op;
        op->io.next = NULL;
        op->io_active = true;
        submit_disk_io(&op->io);

        cd->phase = CD_FIND_SLOTS;
        return;
    }

    case CD_FIND_SLOTS: {
        // Initialize find_slots_state
        cd->fss.dir_cluster = cd->parent_cluster;
        cd->fss.current_cluster = cd->parent_cluster;
        cd->fss.entry_idx = 0;
        cd->fss.needed = cd->total_slots;
        cd->fss.run_start = -1;
        cd->fss.run_len = 0;
        cd->fss.new_cluster = 0;
        cd->fss.tail_traversing = false;
        cd->fss.end_of_dir = false;
        cd->fss.phase = FS_SCAN_CLUSTER;

        int r = find_slots_step(&cd->fss, op, &cd->fws, cd->cluster_buf);
        if (r == FIND_SLOTS_ASYNC) return;

        if (r == FIND_SLOTS_ERROR) {
            op_complete(op, -ENOMEM, 0);
            return;
        }

        if (cd->lfn_count > 0) {
            cd->lfn_written_count = 0;
            cd->phase = CD_WRITE_LFN;
        } else {
            cd->phase = CD_WRITE_SHORT;
        }
        resume_create_dir(op);
        return;
    }

    case CD_WRITE_LFN: {
        // Handle wds progression from previous write
        if (cd->wds.phase > 0 && cd->wds.phase < 3) {
            write_dir_entry_resume(op, &cd->wds);
            if (cd->wds.phase < 3) return; // I/O still in progress
            // phase advanced to 3 (done) — fall through below
        }
        if (cd->wds.phase == 0 && cd->wds.cluster != 0) {
            // Cache miss read completed — re-call write_dir_entry_async
            write_dir_entry_async(op, &cd->wds,
                cd->wds.cluster, cd->wds.index, &cd->wds.entry);
            return;
        }
        if (cd->wds.phase == 3) {
            // Previous LFN entry write done
            cd->wds.cluster = 0; // reset
            cd->lfn_written_count++;
            if (cd->lfn_written_count < cd->lfn_count) {
                // Write next LFN entry
            } else {
                // All LFN entries written
                cd->wds.phase = 0;   // reset so CD_WRITE_SHORT doesn't see stale phase=3
                cd->wds.cluster = 0;
                cd->phase = CD_WRITE_SHORT;
                resume_create_dir(op);
                return;
            }
        }

        // Write one LFN entry
        if (cd->lfn_written_count < cd->lfn_count) {
            int n = cd->lfn_written_count;
            int seq = cd->lfn_count - n;
            bool is_last = (n == 0);

            fat_dir_entry lfn_de;
            uint8_t *raw = (uint8_t *)&lfn_de;
            for (int k = 0; k < 32; k++) raw[k] = 0;
            raw[0] = seq | (is_last ? 0x40 : 0x00);
            raw[11] = 0x0F; raw[12] = 0x00;
            raw[13] = lfn_checksum((const uint8_t *)cd->short_name);
            static const int offsets[] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
            int base = n * 13;
            for (int c = 0; c < 13; c++) {
                int pos = base + c;
                uint16_t ucs2;
                if (pos < cd->leaf_len) ucs2 = (uint16_t)(uint8_t)cd->leaf_name_buf[pos];
                else if (pos == cd->leaf_len) ucs2 = 0x0000;
                else ucs2 = 0xFFFF;
                raw[offsets[c]] = ucs2 & 0xFF;
                raw[offsets[c] + 1] = (ucs2 >> 8) & 0xFF;
            }

            write_dir_entry_async(op, &cd->wds,
                cd->fss.slots[n].cluster, cd->fss.slots[n].index, &lfn_de);
            return;
        }

        // All LFN entries written
        cd->phase = CD_WRITE_SHORT;
        resume_create_dir(op);
        return;
    }

    case CD_WRITE_SHORT: {
        // Handle wds progression from previous write
        if (cd->wds.phase > 0 && cd->wds.phase < 3) {
            write_dir_entry_resume(op, &cd->wds);
            if (cd->wds.phase < 3) return;
            // done — fall through
        }
        if (cd->wds.phase == 0 && cd->wds.cluster != 0) {
            write_dir_entry_async(op, &cd->wds,
                cd->wds.cluster, cd->wds.index, &cd->wds.entry);
            return;
        }
        if (cd->wds.phase == 3) {
            // Short name entry write done
            if (cd->fss.end_of_dir) {
                // Allocated past original end-of-directory — write new 0x00 terminator
                cd->wds.phase = 0;  // reset wds so CD_WRITE_EOD starts fresh
                cd->wds.cluster = 0;
                cd->phase = CD_WRITE_EOD;
            } else {
                cd->phase = CD_DONE;
            }
            resume_create_dir(op);
            return;
        }

        // Construct and write short name entry
        fat_dir_entry new_entry;
        __memcpy(new_entry.name, cd->short_name, 11);
        if (cd->is_mkdir) {
            new_entry.attr = 0x10;
            new_entry.fst_clus_hi = (cd->new_dir_cluster >> 16) & 0xFFFF;
            new_entry.fst_clus_lo = cd->new_dir_cluster & 0xFFFF;
        } else {
            new_entry.attr = 0x20;
            new_entry.fst_clus_hi = 0;
            new_entry.fst_clus_lo = 0;
        }
        new_entry.nt_res = 0;
        new_entry.crt_time_tenth = 0;
        new_entry.crt_time = HARD_TIME;
        new_entry.crt_date = HARD_DATE;
        new_entry.lst_acc_date = HARD_DATE;
        new_entry.wrt_time = HARD_TIME;
        new_entry.wrt_date = HARD_DATE;
        new_entry.file_size = 0;

        write_dir_entry_async(op, &cd->wds,
            cd->fss.slots[cd->lfn_count].cluster,
            cd->fss.slots[cd->lfn_count].index,
            &new_entry);
        return;
    }

    case CD_UPDATE_TIMESTAMP: {
        // Handle wds progression
        if (cd->wds.phase > 0 && cd->wds.phase < 3) {
            write_dir_entry_resume(op, &cd->wds);
            if (cd->wds.phase < 3) return;
            // done — fall through
        }
        if (cd->wds.phase == 0 && cd->wds.cluster != 0) {
            write_dir_entry_async(op, &cd->wds,
                cd->wds.cluster, cd->wds.index, &cd->wds.entry);
            return;
        }
        if (cd->wds.phase == 3) {
            // Timestamp update done
            cd->phase = CD_DONE;
            resume_create_dir(op);
            return;
        }

        // Resolve parent to find the entry location for timestamp update
        // rs already resolved with is_parent=true
        // Now we need to find the entry by leaf_name in parent_cluster
        // Set up resolve_state to scan just parent_cluster for leaf_name
        if (cd->rs.phase == RS_DONE) {
            // Parent resolved — now find the entry in parent dir
            // Scan parent_cluster for leaf_name
            int slot = cache_lookup(cd->parent_cluster);
            if (slot < 0) {
                // Cache miss — async read
                slot = cache_alloc(cd->parent_cluster);
                op->io.lba = data_start_lba + (cd->parent_cluster - 2) * sectors_per_cluster;
                op->io.count = sectors_per_cluster;
                op->io.buf = cache[slot].data;
                op->io.dir = 0;
                op->io.complete = [](disk_io *io) {
                    pending_op *o = (pending_op *)io->ctx;
                    o->io_active = false;
                    o->resume(o);
                };
                op->io.ctx = op;
                op->io.next = NULL;
                op->io_active = true;
                submit_disk_io(&op->io);
                return;
            }

            // Scan parent dir for leaf_name
            uint8_t *data = cache[slot].data;
            int entries = bytes_per_cluster / 32;
            char lb[256]; lb[0] = '\0';
            bool found = false;
            fat_dir_entry found_de;
            uint32_t found_cluster = cd->parent_cluster;
            int found_idx = -1;

            for (int i = 0; i < entries; i++) {
                fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
                if (de->name[0] == 0x00) break;
                if (de->name[0] == 0xE5) { lb[0] = '\0'; continue; }
                if (de->attr == 0x0F) { collect_lfn_entry(de, lb); continue; }
                bool m = false;
                if (lb[0] != '\0') m = match_lfn_name(lb, cd->leaf_name_buf, cd->leaf_len);
                if (!m) m = match_83_name(de->name, cd->leaf_name_buf, cd->leaf_len);
                lb[0] = '\0';
                if (m) { found_de = *de; found_idx = i; found = true; break; }
            }

            if (!found) {
                op_complete(op, -ENOENT, 0);
                return;
            }

            // Update timestamp
            found_de.wrt_date = HARD_DATE;
            found_de.wrt_time = HARD_TIME;
            found_de.lst_acc_date = HARD_DATE;

            write_dir_entry_async(op, &cd->wds,
                found_cluster, found_idx, &found_de);
            return;
        }

        // rs not done — continue resolving parent
        int r = resolve_step(&cd->rs, op);
        if (r == RESOLVE_ASYNC) return;

        if (r == RESOLVE_ERROR || !cd->rs.found) {
            op_complete(op, -ENOENT, 0);
            return;
        }

        cd->parent_cluster = cd->rs.result_cluster;
        // Re-enter to scan parent dir
        resume_create_dir(op);
        return;
    }

    case CD_WRITE_EOD: {
        // Write end-of-directory 0x00 terminator after the new short name entry.
        // The slot after the short entry is slots[total_slots] (total_slots = lfn_count + 1).
        // Handle wds progression from previous write (if resuming after async I/O)
        if (cd->wds.phase > 0 && cd->wds.phase < 3) {
            write_dir_entry_resume(op, &cd->wds);
            if (cd->wds.phase < 3) return;
        }
        if (cd->wds.phase == 0 && cd->wds.cluster != 0) {
            write_dir_entry_async(op, &cd->wds,
                cd->wds.cluster, cd->wds.index, &cd->wds.entry);
            return;
        }
        if (cd->wds.phase == 3) {
            // End-of-directory terminator write done
            cd->phase = CD_DONE;
            resume_create_dir(op);
            return;
        }

        // Determine the slot for the 0x00 terminator: it's the slot right after the short entry
        int eod_slot_idx = cd->total_slots;  // total_slots = lfn_count + 1
        uint32_t eod_cluster;
        int eod_index;

        if (eod_slot_idx < cd->fss.needed + 1) {
            // Check if the slot after short entry is still within our allocated run
            // The allocated run is slots[0..needed-1], so the EOD goes at index = needed
            // (which is one past the short entry = slots[needed-1])
            eod_cluster = cd->fss.slots[cd->fss.needed - 1].cluster;
            eod_index = cd->fss.slots[cd->fss.needed - 1].index + 1;
        } else {
            // Shouldn't happen, but handle gracefully
            eod_cluster = cd->fss.slots[cd->lfn_count].cluster;
            eod_index = cd->fss.slots[cd->lfn_count].index + 1;
        }

        // Check if eod_index is within the same cluster
        int epc = bytes_per_cluster / 32;
        if (eod_index >= epc) {
            // The EOD would be in the next cluster. Since end_of_dir was true,
            // the entry after the 0x00 marker should already be 0x00 (FAT32 spec),
            // and if we're at the last entry in the cluster, the next cluster (if any)
            // starts with 0x00 entries. If there's no next cluster, we'd need to
            // extend — but for simplicity, if we're at the cluster boundary, the
            // next cluster (if it exists) is already zeroed. Skip writing EOD.
            cd->phase = CD_DONE;
            resume_create_dir(op);
            return;
        }

        // Write a zero entry (0x00 in name[0] = end-of-directory marker)
        fat_dir_entry eod_entry;
        __memset(&eod_entry, 0, 32);
        write_dir_entry_async(op, &cd->wds, eod_cluster, eod_index, &eod_entry);
        return;
    }

    case CD_DONE:
        write_lock_release();
        resp->status = 0;
        op_complete(op, 0, 0);
        return;

    default:
        op_complete(op, -EIO, 0);
        return;
    }
}

// ---- Sync helpers for create/mkdir ----
// These use the sync disk I/O and are called from within the event loop
// during create/mkdir processing. They block the event loop briefly but
// writes are single-sector operations that complete quickly.

// next_free_hint defined at top of file

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


// ===================== FAT32 init (sync) =====================
static void fat32_init() {
    uint8_t mbr_buf[512];
    if (disk_read_sync(0, 1, mbr_buf) != 0) {
        while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    }
    uint8_t *mbr = mbr_buf;

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


    disk_read_sync(part_start_lba, 1, mbr_buf);
    uint8_t *bpb = mbr_buf;

    uint16_t bps = (uint16_t)bpb[11] | ((uint16_t)bpb[12] << 8);
    sectors_per_cluster = bpb[13];
    uint16_t reserved = (uint16_t)bpb[14] | ((uint16_t)bpb[15] << 8);
    spf32 = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
            ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    root_cluster = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                   ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);

    if (bps != 512 || sectors_per_cluster == 0 || spf32 == 0 || root_cluster < 2) {
        const char msg[] = "EBPB\n";
        while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    }

    fat_start_lba = part_start_lba + reserved;
    data_start_lba = fat_start_lba + spf32 * 2;
    bytes_per_cluster = sectors_per_cluster * 512;

    uint32_t data_sectors = part_total_sectors - (data_start_lba - part_start_lba);
    total_data_clusters = data_sectors / sectors_per_cluster;

    for (uint32_t s = 0; s < spf32; s++) {
        fat_cache_read_sync(fat_start_lba + s);
    }
}

// ===================== Main event loop =====================
extern "C" void _start() {

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

    // Sync init (MBR/BPB/FAT cache warm-up)
    fat32_init();

    device_register(getpid(), DEV_FS);

    // Event loop
    while (1) {
        struct recv_msg m;
        uint8_t data_buf[sizeof(struct file_req) + 65536];
        int rr = recv(&m, data_buf, sizeof(data_buf), 0);
        if (rr != 0) continue;

        // Notifications: disk I/O completion, write_lock release, etc.
        if (m.type == RECV_NOTIFY) {
            // Parse AHCI completion data: cookie(4) + result(4) + lba(4) + count(4)
            uint32_t cookie, result, lba, count;
            memcpy(&cookie, m.data, 4);
            memcpy(&result, m.data + 4, 4);
            memcpy(&lba, m.data + 8, 4);
            memcpy(&count, m.data + 12, 4);

            // Check readahead completion first
            if (readahead_pending && readahead_io.cookie == cookie) {
                readahead_pending = false;
                continue;
            }

            // Find pending_op with matching cookie
            pending_op *op = find_pending_op_by_cookie(cookie);
            if (op && op->io_active) {
                op->io_active = false;
                op->io.complete(&op->io);
            }
            continue;
        }

        if (m.type != RECV_MSG) continue;

        pid_t client_pid = (pid_t)m.src;

        struct file_req *freq = (struct file_req *)data_buf;

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

        case FILE_CMD_CLOSEDIR: {
            // Close directory session fd — synchronous
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
            if (freq->fs_fd < MAX_SESSION_FDS) {
                sess->open_files[freq->fs_fd].used = false;
                sess->open_files[freq->fs_fd].is_dir_session = false;
            }
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_SEEK: {
            // Update session fd offset — synchronous
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
            if (freq->fs_fd < MAX_SESSION_FDS && sess->open_files[freq->fs_fd].used) {
                session_open_file *f = &sess->open_files[freq->fs_fd];
                uint32_t new_ofs;
                switch (freq->flags) { // reuse flags field for whence (SEEK_SET/CUR/END)
                case 0: new_ofs = (uint32_t)freq->offset; break;        // SEEK_SET
                case 1: new_ofs = (uint32_t)(f->offset + freq->offset); break; // SEEK_CUR
                case 2: new_ofs = (uint32_t)(f->file_size + freq->offset); break; // SEEK_END
                default: resp.status = -EINVAL;
                }
                if (resp.status == 0) f->offset = new_ofs;
            } else { resp.status = -EBADF; }
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_ACCESS: {
            // Check if file exists — synchronous stat path check
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
            // Quick sync check: resolve path non-recurisve
            resolve_state rs;
            memset(&rs, 0, sizeof(rs));
            rs.path = freq->path;
            rs.path_pos = 0; rs.comp_start = 0; rs.comp_len = 0;
            rs.dir_cluster = root_cluster; rs.current_cluster = root_cluster;
            rs.entry_idx = 0; rs.lfn_buf[0] = '\0';
            rs.found = false; rs.is_parent = false; rs.leaf_len = 0;
            rs.phase = RS_INIT;
            int r = resolve_step(&rs, NULL);
            if (r == RESOLVE_ERROR || !rs.found) { resp.status = -ENOENT; }
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_FSTAT: {
            // Stat by session fd — synchronous
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
            if (freq->fs_fd < MAX_SESSION_FDS && sess->open_files[freq->fs_fd].used) {
                session_open_file *f = &sess->open_files[freq->fs_fd];
                resp.file_size = f->file_size;
                // Also encode attr in fd field for simplicity
                resp.fd = f->is_dir_session ? 0x10 : 0x20;
            } else { resp.status = -EBADF; }
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_OPENDIR: {
            // Open directory — resolve path, allocate session fd
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;

            // Resolve path synchronously
            resolve_state rs;
            memset(&rs, 0, sizeof(rs));
            rs.path = freq->path;
            rs.path_pos = 0; rs.comp_start = 0; rs.comp_len = 0;
            rs.dir_cluster = root_cluster; rs.current_cluster = root_cluster;
            rs.entry_idx = 0; rs.lfn_buf[0] = '\0';
            rs.found = false; rs.is_parent = false; rs.leaf_len = 0;
            rs.phase = RS_INIT;

            // Resolve path but we may need sync I/O — use sync fallback
            // Check root first
            if (freq->path[0] == '/' && freq->path[1] == '\0') {
                // Root directory
                int fd = session_alloc_fd(sess);
                if (fd < 0) { resp.status = -EMFILE; msg_resp(&resp, sizeof(resp)); break; }
                sess->open_files[fd].used = true;
                sess->open_files[fd].is_dir_session = true;
                sess->open_files[fd].start_cluster = root_cluster;
                sess->open_files[fd].dir_cur_cluster = root_cluster;
                sess->open_files[fd].dir_entry_idx = 0;
                sess->open_files[fd].offset = 0;
                resp.fd = (uint32_t)fd;
                msg_resp(&resp, sizeof(resp));
                break;
            }

            // Synchronous path resolution using sync disk I/O
            rs.phase = RS_INIT;
            int did_async = 0;
            while (1) {
                int r2 = resolve_step(&rs, NULL);
                if (r2 == RESOLVE_ASYNC) {
                    // Need sync I/O: use a simple retry approach
                    // Since we can't do async, just try cache
                    if (rs.phase == RS_READ_CLUSTER) {
                        int slot = read_cluster_sync(rs.current_cluster);
                        if (slot < 0) { resp.status = -EIO; break; }
                        rs.phase = RS_SCAN_ENTRIES;
                        continue;
                    }
                    if (rs.phase == RS_READ_FAT) {
                        uint32_t fat_offset = rs.current_cluster * 4;
                        uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                        int slot = fat_cache_read_sync(fat_sector);
                        if (slot < 0) { resp.status = -EIO; break; }
                        continue;
                    }
                    resp.status = -EIO; break;
                }
                if (r2 == RESOLVE_ERROR || !rs.found) { resp.status = -ENOENT; break; }
                // Found
                if (!(rs.result.attr & 0x10)) { resp.status = -ENOTDIR; break; }
                uint32_t dir_cluster = ((uint32_t)rs.result.fst_clus_hi << 16) | rs.result.fst_clus_lo;
                if (dir_cluster == 0) dir_cluster = root_cluster;
                int fd = session_alloc_fd(sess);
                if (fd < 0) { resp.status = -EMFILE; break; }
                sess->open_files[fd].used = true;
                sess->open_files[fd].is_dir_session = true;
                sess->open_files[fd].start_cluster = dir_cluster;
                sess->open_files[fd].dir_cur_cluster = dir_cluster;
                sess->open_files[fd].dir_entry_idx = 0;
                sess->open_files[fd].offset = 0;
                resp.fd = (uint32_t)fd;
                break;
            }
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_DIRENT: {
            // Read next dirent from directory session fd — synchronous
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;

            if (freq->fs_fd >= MAX_SESSION_FDS || !sess->open_files[freq->fs_fd].used ||
                !sess->open_files[freq->fs_fd].is_dir_session) {
                resp.status = -EBADF;
                msg_resp(&resp, sizeof(resp));
                break;
            }

            session_open_file *d = &sess->open_files[freq->fs_fd];
            fs_dirent *out = (fs_dirent *)resp.data;
            uint32_t out_count = 0;
            uint32_t max_out = (MAX_REPLY_DATA - sizeof(file_resp)) / sizeof(fs_dirent);
            if (max_out > 8) max_out = 8; // limit per call

            // Scan directory entries
            while (out_count < max_out) {
                uint32_t cc = d->dir_cur_cluster;
                if (cc < 2 || cc >= 0x0FFFFFF8) break; // end of chain

                int slot = cache_lookup(cc);
                if (slot < 0) {
                    // Sync read
                    slot = read_cluster_sync(cc);
                    if (slot < 0) { resp.status = -EIO; break; }
                }

                uint8_t *data = cache[slot].data;
                int entries = bytes_per_cluster / 32;
                bool advanced = false;

                for (; d->dir_entry_idx < entries; d->dir_entry_idx++) {
                    fat_dir_entry *de = (fat_dir_entry *)(data + d->dir_entry_idx * 32);
                    if (de->name[0] == 0x00) { goto dirent_done; }
                    if (de->name[0] == 0xE5) { d->dir_entry_lfn[0] = 0; continue; }
                    if (de->attr == 0x0F) {
                        collect_lfn_entry(de, d->dir_entry_lfn);
                        continue;
                    }
                    if (de->name[0] == '.') {
                        d->dir_entry_lfn[0] = 0;
                        continue;
                    }

                    // Valid entry — output
                    fs_dirent *e = &out[out_count];
                    if (d->dir_entry_lfn[0] != 0) {
                        int j = 0;
                        while (d->dir_entry_lfn[j] && j < 255) { e->name[j] = d->dir_entry_lfn[j]; j++; }
                        e->name[j] = '\0';
                    } else {
                        convert_83_to_name(de->name, de->nt_res, e->name, 256);
                    }
                    e->size = de->file_size;
                    e->date = de->wrt_date;
                    e->time = de->wrt_time;
                    e->attr = de->attr;
                    out_count++;
                    d->dir_entry_lfn[0] = 0;
                    advanced = true;
                    break; // return one entry per call for simplicity
                }

                if (!advanced) {
                    // Move to next cluster
                    uint32_t next = fat_read_entry_cached(cc);
                    if (next == 0xFFFFFFFF) {
                        uint32_t fat_offset = cc * 4;
                        uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                        if (fat_cache_read_sync(fat_sector) < 0) { break; }
                        continue;
                    }
                    if (next >= 0x0FFFFFF8) break;
                    d->dir_cur_cluster = next;
                    d->dir_entry_idx = 0;
                }
            }

dirent_done:
            resp.count = out_count * sizeof(fs_dirent);
            resp.total = out_count;
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_UNLINK: {
            // Delete file — synchronous implementation
            // Resolve path, mark entry 0xE5, free FAT chain
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;

            // Resolve parent sync
            resolve_state rs;
            memset(&rs, 0, sizeof(rs));
            rs.path = freq->path;
            rs.path_pos = 0; rs.comp_start = 0; rs.comp_len = 0;
            rs.dir_cluster = root_cluster; rs.current_cluster = root_cluster;
            rs.entry_idx = 0; rs.lfn_buf[0] = '\0';
            rs.found = false; rs.is_parent = true; rs.leaf_len = 0;
            rs.phase = RS_INIT;

            while (1) {
                int r2 = resolve_step(&rs, NULL);
                if (r2 == RESOLVE_ASYNC) {
                    if (rs.phase == RS_READ_CLUSTER) {
                        int slot = read_cluster_sync(rs.current_cluster);
                        if (slot < 0) { resp.status = -EIO; break; }
                        rs.phase = RS_SCAN_ENTRIES; continue;
                    }
                    if (rs.phase == RS_READ_FAT) {
                        uint32_t fat_offset = rs.current_cluster * 4;
                        uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                        if (fat_cache_read_sync(fat_sector) < 0) { resp.status = -EIO; break; }
                        continue;
                    }
                    resp.status = -EIO; break;
                }
                if (r2 == RESOLVE_ERROR || !rs.found) { resp.status = -ENOENT; break; }

                // Found parent or entry itself
                uint32_t parent_cluster = rs.result_cluster;
                fat_dir_entry target = rs.result;
                uint32_t target_cluster = ((uint32_t)target.fst_clus_hi << 16) | target.fst_clus_lo;

                if (target.attr & 0x10) { resp.status = -EISDIR; break; }

                // Mark entry as deleted (0xE5)
                int slot = cache_lookup(parent_cluster);
                if (slot < 0) {
                    slot = read_cluster_sync(parent_cluster);
                    if (slot < 0) { resp.status = -EIO; break; }
                }
                uint8_t *entry_data = cache[slot].data + rs.result_entry_idx * 32;
                entry_data[0] = 0xE5; // mark deleted

                // Write back affected sector
                int sector_idx = (rs.result_entry_idx * 32) / 512;
                uint32_t clba = data_start_lba + (parent_cluster - 2) * sectors_per_cluster;
                disk_write_sync(clba + sector_idx, 1, cache[slot].data + sector_idx * 512);

                // Free FAT cluster chain
                if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8) {
                    uint32_t c = target_cluster;
                    while (c >= 2 && c < 0x0FFFFFF8) {
                        uint32_t next = fat_read_entry_sync(c);
                        // Mark cluster as free
                        fat_write_state fws;
                        memset(&fws, 0, sizeof(fws));
                        fws.cluster = c;
                        fws.value = 0;
                        fat_write_state fws_done;
                        memset(&fws_done, 0, sizeof(fws_done));
                        {
                            uint32_t fat_offset2 = c * 4;
                            uint32_t fat_sector2 = fat_start_lba + (fat_offset2 / 512);
                            uint32_t offset_in_sector2 = fat_offset2 % 512;
                            uint8_t sector_buf[512];
                            disk_read_sync(fat_sector2, 1, sector_buf);
                            uint8_t *p = sector_buf + offset_in_sector2;
                            uint32_t old = (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
                            uint32_t nv = old & 0xF0000000; // clear 28 bits, keep upper nibble
                            p[0]=nv&0xFF; p[1]=(nv>>8)&0xFF; p[2]=(nv>>16)&0xFF; p[3]=(nv>>24)&0xFF;
                            disk_write_sync(fat_sector2, 1, sector_buf);
                            // Write FAT2
                            disk_write_sync(fat_sector2 + spf32, 1, sector_buf);
                        }
                        cache_invalidate(c);
                        if (next >= 0x0FFFFFF8) break;
                        c = next;
                    }
                }

                // Update next_free_hint if target_cluster is lower
                if (target_cluster >= 2 && target_cluster < next_free_hint)
                    next_free_hint = target_cluster;

                break;
            }
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_RMDIR: {
            // Remove empty directory — synchronous
            struct file_resp resp;
            resp.status = 0; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;

            resolve_state rs;
            memset(&rs, 0, sizeof(rs));
            rs.path = freq->path;
            rs.path_pos = 0; rs.comp_start = 0; rs.comp_len = 0;
            rs.dir_cluster = root_cluster; rs.current_cluster = root_cluster;
            rs.entry_idx = 0; rs.lfn_buf[0] = '\0';
            rs.found = false; rs.is_parent = false; rs.leaf_len = 0;
            rs.phase = RS_INIT;

            while (1) {
                int r2 = resolve_step(&rs, NULL);
                if (r2 == RESOLVE_ASYNC) {
                    if (rs.phase == RS_READ_CLUSTER) {
                        int slot = read_cluster_sync(rs.current_cluster);
                        if (slot < 0) { resp.status = -EIO; break; }
                        rs.phase = RS_SCAN_ENTRIES; continue;
                    }
                    if (rs.phase == RS_READ_FAT) {
                        uint32_t fat_offset = rs.current_cluster * 4;
                        uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
                        if (fat_cache_read_sync(fat_sector) < 0) { resp.status = -EIO; break; }
                        continue;
                    }
                    resp.status = -EIO; break;
                }
                if (r2 == RESOLVE_ERROR || !rs.found) { resp.status = -ENOENT; break; }

                fat_dir_entry target = rs.result;
                uint32_t target_cluster = ((uint32_t)target.fst_clus_hi << 16) | target.fst_clus_lo;
                if (target_cluster == 0) target_cluster = root_cluster;

                if (!(target.attr & 0x10)) { resp.status = -ENOTDIR; break; }

                // Check directory is empty (only . and ..)
                bool is_empty = true;
                uint32_t cc = target_cluster;
                while (cc >= 2 && cc < 0x0FFFFFF8 && is_empty) {
                    int slot = cache_lookup(cc);
                    if (slot < 0) {
                        slot = read_cluster_sync(cc);
                        if (slot < 0) { resp.status = -EIO; break; }
                    }
                    uint8_t *data = cache[slot].data;
                    int entries = bytes_per_cluster / 32;
                    for (int i = 0; i < entries; i++) {
                        fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
                        if (de->name[0] == 0x00) break;
                        if (de->name[0] == 0xE5) continue;
                        if (de->name[0] == '.' && (de->name[1] == ' ' || de->name[1] == '.')) continue;
                        if (de->name[0] == '.' && de->name[1] == '.') continue;
                        is_empty = false;
                        break;
                    }
                    // Next cluster
                    uint32_t next = fat_read_entry_sync(cc);
                    if (next >= 0x0FFFFFF8) break;
                    cc = next;
                }

                if (!is_empty) { resp.status = -EBUSY; break; }

                // Mark entry deleted (0xE5) in parent
                uint32_t parent_cluster = rs.result_cluster;
                int pslot = cache_lookup(parent_cluster);
                if (pslot < 0) {
                    pslot = read_cluster_sync(parent_cluster);
                    if (pslot < 0) { resp.status = -EIO; break; }
                }
                uint8_t *entry_data = cache[pslot].data + rs.result_entry_idx * 32;
                entry_data[0] = 0xE5;
                int sector_idx = (rs.result_entry_idx * 32) / 512;
                uint32_t plba = data_start_lba + (parent_cluster - 2) * sectors_per_cluster;
                disk_write_sync(plba + sector_idx, 1, cache[pslot].data + sector_idx * 512);

                // Free directory cluster chain
                if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8) {
                    uint32_t c = target_cluster;
                    while (c >= 2 && c < 0x0FFFFFF8) {
                        uint32_t next = fat_read_entry_sync(c);
                        uint32_t fat_offset2 = c * 4;
                        uint32_t fat_sector2 = fat_start_lba + (fat_offset2 / 512);
                        uint32_t offset_in_sector2 = fat_offset2 % 512;
                        uint8_t sector_buf[512];
                        disk_read_sync(fat_sector2, 1, sector_buf);
                        uint8_t *p = sector_buf + offset_in_sector2;
                        uint32_t old = (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
                        uint32_t nv = old & 0xF0000000;
                        p[0]=nv&0xFF; p[1]=(nv>>8)&0xFF; p[2]=(nv>>16)&0xFF; p[3]=(nv>>24)&0xFF;
                        disk_write_sync(fat_sector2, 1, sector_buf);
                        disk_write_sync(fat_sector2 + spf32, 1, sector_buf);
                        cache_invalidate(c);
                        if (next >= 0x0FFFFFF8) break;
                        c = next;
                    }
                }

                if (target_cluster >= 2 && target_cluster < next_free_hint)
                    next_free_hint = target_cluster;

                break;
            }
            msg_resp(&resp, sizeof(resp));
            break;
        }

        case FILE_CMD_WRITE: {
            size_t msg_data_len = m.msg.len;
            size_t write_data_len = (msg_data_len > sizeof(file_req)) ? msg_data_len - sizeof(file_req) : 0;
            uint8_t *write_data = (write_data_len > 0) ? data_buf + sizeof(file_req) : NULL;

            // Use the offset from the request (kernel proxy or libc direct)
            if (freq->fs_fd < MAX_SESSION_FDS && sess->open_files[freq->fs_fd].used) {
                sess->open_files[freq->fs_fd].offset = freq->offset;
            }

            pending_op *op = alloc_pending_op();
            if (!op) {
                struct file_resp resp;
                resp.status = -ENOMEM; resp.fd = 0; resp.file_size = 0; resp.count = 0; resp.total = 0;
                msg_resp(&resp, sizeof(resp));
                break;
            }
            op->client_pid = client_pid;
            op->session_idx = sess_idx;
            op->type = OP_WRITE;
            op->io_active = false;
            start_write(op, sess, freq->fs_fd, freq->count, write_data, write_data_len);
            break;
        }

        case FILE_CMD_READ: {
            // Use the offset from the request (kernel proxy or libc direct)
            if (freq->fs_fd < MAX_SESSION_FDS && sess->open_files[freq->fs_fd].used) {
                sess->open_files[freq->fs_fd].offset = freq->offset;
            }

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
            start_read(op, sess, freq->fs_fd, freq->count);
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
            start_create_dir(op, freq->path, false);
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
            start_create_dir(op, freq->path, true);
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
