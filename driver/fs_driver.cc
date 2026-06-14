// ===================== FAT32 filesystem driver (user-space) — read + write + LFN
// Reads requests from FS_REQ shared page, performs FAT32 operations,
// writes results in FS_RESP. Accesses disk via disk_driver through
// DISK_REQ/DISK_RESP shared pages.
#include <stdint.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/device.h>
#include <sys/serial.h>
#include "common/shm.h"
#include "common/dev.h"

// Static buffers to avoid stack overflow (4KB arrays on stack exceed user stack page)
static uint8_t dir_cluster_data_buf[4096];   // used in handle_mkdir
static uint8_t zero_cluster_buf[4096];       // used in dir_chain_extend
static uint8_t sector_buf[512];              // used in fat_write_entry

// ===================== Shared page pointers (set in _start) =====================
static volatile disk_req_shm  *dreq;
static volatile disk_resp_shm *dresp;
static volatile disk_shm_header *disk_hdr;
static volatile fs_req_shm    *freq;
static volatile fs_resp_shm   *fresp;
static volatile fs_shm_header *fs_hdr;

// ===================== FAT32 volume state =====================
static uint32_t part_start_lba;     // FAT32 partition start LBA
static uint32_t fat_start_lba;      // First FAT sector (absolute)
static uint32_t data_start_lba;     // First data sector (absolute)
static uint32_t root_cluster;       // Root directory cluster
static uint32_t sectors_per_cluster;
static uint32_t bytes_per_cluster;  // = sectors_per_cluster * 512
static uint32_t total_data_clusters; // total data clusters in volume
static uint32_t spf32;              // sectors per FAT copy (cached at init)

// ===================== Disk I/O helpers =====================
static int32_t disk_driver_pid;

static void disk_wait_reply() {
    int32_t my_pid = getpid();
    while (1) {
        struct recv_msg m;
        recv(&m, 0);
        if (m.type == RECV_NOTIFY && (int32_t)m.src == disk_driver_pid) return;
        notify(my_pid);
    }
}

static int disk_read(uint32_t lba, uint32_t count) {
    dreq->cmd   = DISK_CMD_READ;
    dreq->lba   = lba;
    dreq->count = count;
    disk_hdr->fs_driver_sleeping = 1;
    if (disk_hdr->disk_driver_sleeping) {
        notify(disk_driver_pid);
    }
    disk_wait_reply();
    disk_hdr->fs_driver_sleeping = 0;
    return dresp->status;
}

static int disk_write(uint32_t lba, uint32_t count, const uint8_t *data) {
    dreq->cmd   = DISK_CMD_WRITE;
    dreq->lba   = lba;
    dreq->count = count;
    uint32_t bytes = count * 512;
    __memcpy((void *)dreq->data, data, bytes);
    disk_hdr->fs_driver_sleeping = 1;
    if (disk_hdr->disk_driver_sleeping) {
        notify(disk_driver_pid);
    }
    disk_wait_reply();
    disk_hdr->fs_driver_sleeping = 0;
    return dresp->status;
}

// ===================== Data cluster LRU cache (8 slots) =====================
#define CACHE_SLOTS 8

struct cache_entry {
    uint32_t cluster;   // 0xFFFFFFFF = invalid
    uint8_t  data[4096]; // one cluster (4KB with -s 8)
    uint32_t age;
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
    int best = 0;
    for (int i = 1; i < CACHE_SLOTS; i++) {
        if (cache[i].age < cache[best].age) best = i;
    }
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

static int read_cluster(uint32_t cluster) {
    int slot = cache_lookup(cluster);
    if (slot >= 0) return slot;

    slot = cache_alloc(cluster);
    uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;

    if (disk_read(lba, sectors_per_cluster) != 0) {
        cache[slot].cluster = 0xFFFFFFFF;
        return -1;
    }

    __memcpy(cache[slot].data, (const void *)dresp->data, bytes_per_cluster);
    return slot;
}

// ===================== FAT sector cache (4 pages, fixed) =====================
#define FAT_CACHE_PAGES 4

struct fat_cache_entry {
    uint32_t sector_lba; // absolute LBA of this FAT sector
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

// Read a FAT sector through cache. Returns cache index or -1.
static int fat_cache_read(uint32_t sector_lba) {
    int slot = fat_cache_lookup(sector_lba);
    if (slot >= 0) return slot;

    if (disk_read(sector_lba, 1) != 0) return -1;

    slot = fat_cache_alloc(sector_lba);
    __memcpy(fat_cache[slot].data, (const void *)dresp->data, 512);
    return slot;
}

// ===================== FAT chain traversal =====================
static uint32_t fat_read_entry(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;

    int slot = fat_cache_read(fat_sector);
    if (slot < 0) return 0x0FFFFFFF;

    uint8_t *src = fat_cache[slot].data + offset_in_sector;
    uint32_t entry_val = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                         ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
    return entry_val & 0x0FFFFFFF;
}

static int fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;

    // Read the FAT sector into cache (or use cached copy)
    int slot = fat_cache_read(fat_sector);
    if (slot < 0) return 1;

    // Also keep a copy in sector_buf for disk write
    __memcpy(sector_buf, fat_cache[slot].data, 512);

    // Modify the 4-byte FAT entry
    uint8_t *p = sector_buf + offset_in_sector;
    uint32_t old_val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t new_val = (old_val & 0xF0000000) | (value & 0x0FFFFFFF);
    p[0] = new_val & 0xFF;
    p[1] = (new_val >> 8) & 0xFF;
    p[2] = (new_val >> 16) & 0xFF;
    p[3] = (new_val >> 24) & 0xFF;

    // Write FAT1
    if (disk_write(fat_sector, 1, sector_buf) != 0) return 2;

    // Write FAT2
    uint32_t fat2_sector = fat_sector + spf32;
    if (disk_write(fat2_sector, 1, sector_buf) != 0) return 3;

    // Update FAT cache with the modified data
    __memcpy(fat_cache[slot].data, sector_buf, 512);

    return 0;
}

// ===================== Cluster allocation =====================
static uint32_t next_free_hint = 2; // cursor for find_free_cluster

static uint32_t find_free_cluster() {
    for (uint32_t sector = 0; sector < spf32; sector++) {
        uint32_t abs_sector = ((next_free_hint / 128) + sector) % spf32;
        int slot = fat_cache_read(fat_start_lba + abs_sector);
        if (slot < 0) continue;

        uint8_t *fat_data = fat_cache[slot].data;
        for (int i = 0; i < 128; i++) {
            uint32_t cluster = abs_sector * 128 + i;
            if (cluster < 2) continue;
            if (cluster >= total_data_clusters + 2) continue;

            uint32_t entry = (uint32_t)fat_data[i*4] | ((uint32_t)fat_data[i*4+1] << 8) |
                             ((uint32_t)fat_data[i*4+2] << 16) | ((uint32_t)fat_data[i*4+3] << 24);
            entry &= 0x0FFFFFFF;
            if (entry == 0) {
                next_free_hint = cluster + 1;
                if (next_free_hint >= total_data_clusters + 2) next_free_hint = 2;
                return cluster;
            }
        }
    }
    return 0;
}

static uint32_t allocate_cluster() {
    uint32_t cluster = find_free_cluster();
    if (cluster == 0) return 0;
    if (fat_write_entry(cluster, 0x0FFFFFFF) != 0) return 0;
    return cluster;
}

static int dir_chain_extend(uint32_t dir_cluster, uint32_t *new_cluster_out) {
    uint32_t new_cluster = allocate_cluster();
    if (new_cluster == 0) return 4;

    for (int i = 0; i < bytes_per_cluster; i++) zero_cluster_buf[i] = 0;
    uint32_t new_lba = data_start_lba + (new_cluster - 2) * sectors_per_cluster;
    if (disk_write(new_lba, sectors_per_cluster, zero_cluster_buf) != 0) return 5;

    uint32_t cur = dir_cluster;
    uint32_t prev = cur;
    while (cur >= 2 && cur < 0x0FFFFFF8) {
        prev = cur;
        cur = fat_read_entry(cur);
    }
    if (fat_write_entry(prev, new_cluster) != 0) return 6;

    cache_invalidate(dir_cluster);
    *new_cluster_out = new_cluster;
    return 0;
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
    // Special handling for . and ..
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

// Legacy null-terminated version
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
            // nt_res bit 3 (0x08): basename is lowercase
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
                // nt_res bit 4 (0x10): extension is lowercase
                if ((nt_res & 0x10) && c >= 'A' && c <= 'Z') c += 32;
                out[j++] = c;
            }
        }
    }
    out[j] = '\0';
}

// Case-insensitive compare of name with an 8.3 stored name
static bool match_83_name(const char stored[11], const char *name, int name_len) {
    char expanded[11];
    format_83_name(name, name_len, expanded);
    for (int i = 0; i < 11; i++) {
        if (stored[i] != expanded[i]) return false;
    }
    return true;
}

// ===================== LFN helpers =====================

// Collect LFN characters from a single LFN entry (attr=0x0F)
// into lfn_buf. Returns false if any non-ASCII char found (discard entire LFN).
static bool collect_lfn_entry(const fat_dir_entry *de, char *lfn_buf) {
    const uint8_t *raw = (const uint8_t *)de;
    int seq = raw[0] & 0x3F; // sequence number (1-20)

    // UCS-2 positions within the 32-byte entry:
    // chars 1-5  at offsets 1-10  (5 chars, 10 bytes)
    // chars 6-11 at offsets 14-25 (6 chars, 12 bytes)
    // chars 12-13 at offsets 28-31 (2 chars, 4 bytes)
    static const int offsets[] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    static const int n_chars = 13;

    int base = (seq - 1) * n_chars;

    for (int c = 0; c < n_chars; c++) {
        uint8_t lo = raw[offsets[c]];
        uint8_t hi = raw[offsets[c] + 1];

        if (hi != 0) {
            // Non-ASCII: mark entire LFN as invalid
            lfn_buf[0] = '\0';
            return false;
        }

        if (lo == 0x00 || lo == 0xFF) {
            // Null terminator or padding — end of name
            lfn_buf[base + c] = '\0';
            // Fill remaining with null
            for (int k = base + c + 1; k < 256; k++) lfn_buf[k] = '\0';
            return true;
        }

        lfn_buf[base + c] = (char)lo;
    }
    return true;
}

// Case-insensitive compare of a name (ptr+length) with an LFN buffer
static bool match_lfn_name(const char *lfn_buf, const char *name, int name_len) {
    // Compare character by character, case-insensitive
    for (int i = 0; i < name_len; i++) {
        char lc = lfn_buf[i];
        char nc = name[i];
        if (lc == '\0') return false; // LFN shorter than name
        // toupper both
        if (lc >= 'a' && lc <= 'z') lc -= 32;
        if (nc >= 'a' && nc <= 'z') nc -= 32;
        if (lc != nc) return false;
    }
    // Name matched for name_len chars; LFN must end (null or different)
    // Actually just check that LFN has exactly name_len chars
    return lfn_buf[name_len] == '\0';
}

// LFN checksum (FAT32 spec)
static uint8_t lfn_checksum(const uint8_t *name) {
    uint8_t sum = 0;
    for (int i = 11; i; i--)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *name++;
    return sum;
}

// Forward declarations (ordering dependency: generate_short_name needs find_dir_entry,
// write_lfn_entries needs write_dir_entry_at)
static bool find_dir_entry(uint32_t dir_cluster, const char *name,
                           int name_len, fat_dir_entry *out);
static int write_dir_entry_at(uint32_t cluster, int index, const fat_dir_entry *entry);

// Check if a name is a valid 8.3 name (all uppercase, no special chars, fits 8.3)
static bool is_valid_83(const char *name, int name_len) {
    if (name_len == 0 || name_len > 12) return false;

    int dot_pos = -1;
    for (int i = 0; i < name_len; i++) {
        if (name[i] == '.') {
            if (dot_pos >= 0) return false; // multiple dots
            dot_pos = i;
        }
    }

    int base_len, ext_len;
    if (dot_pos < 0) {
        base_len = name_len;
        ext_len = 0;
    } else {
        base_len = dot_pos;
        ext_len = name_len - dot_pos - 1;
        if (base_len == 0 || ext_len > 3) return false;
    }
    if (base_len > 8) return false;

    // All chars must be uppercase alphanumeric or valid 8.3 chars
    for (int i = 0; i < name_len; i++) {
        char c = name[i];
        if (c == '.') continue;
        if (c >= 'a' && c <= 'z') return false; // lowercase not valid 8.3
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        // Valid 8.3 special chars
        if (c == ' ' || c == '!' || c == '#' || c == '$' || c == '%' ||
            c == '&' || c == '\'' || c == '(' || c == ')' || c == '-' ||
            c == '@' || c == '^' || c == '_' || c == '`' || c == '{' ||
            c == '}' || c == '~') continue;
        return false;
    }
    return true;
}

// Generate 8.3 short name from long name
// Returns number of tilde digits used (0 for valid 8.3, >0 for numeric tail)
static int generate_short_name(const char *name, int name_len, char out[11],
                                uint32_t dir_cluster) {
    if (is_valid_83(name, name_len)) {
        format_83_name(name, name_len, out);
        return 0;
    }

    // Convert to uppercase for short name
    // Take first 6 chars of basename (before dot), add ~N
    char basename[9] = {};
    char extname[4] = {};

    // Find dot position
    int dot_pos = -1;
    for (int i = name_len - 1; i >= 0; i--) {
        if (name[i] == '.') { dot_pos = i; break; }
    }

    // Basename: first 6 uppercase chars
    int blen = (dot_pos >= 0) ? dot_pos : name_len;
    if (blen > 6) blen = 6;
    for (int i = 0; i < blen; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        // Replace invalid 8.3 chars with underscore
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
        basename[i] = c;
    }

    // Extension: first 3 uppercase chars after dot
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

    // Try ~1 through ~9, then ~10 through ~99
    for (int n = 1; n <= 99; n++) {
        // Build candidate short name
        char candidate[11];
        for (int i = 0; i < 11; i++) candidate[i] = ' ';

        // Write "BASENA~N"
        int tilde_len = (n >= 10) ? 3 : 2; // "~1" or "~10"
        int base_chars = 8 - tilde_len; // chars before tilde
        if (base_chars > 6) base_chars = 6;

        int pos = 0;
        for (int i = 0; i < base_chars && basename[i]; i++)
            candidate[pos++] = basename[i];
        // Write tilde and number
        candidate[pos++] = '~';
        if (n >= 10) {
            candidate[pos++] = '0' + (n / 10);
            candidate[pos++] = '0' + (n % 10);
        } else {
            candidate[pos++] = '0' + n;
        }
        // Extension at offset 8
        for (int i = 0; i < 3 && extname[i]; i++)
            candidate[8 + i] = extname[i];

        // Check if this short name already exists in directory
        fat_dir_entry dummy;
        if (!find_dir_entry(dir_cluster, candidate, 11, &dummy)) {
            // Not found — this short name is unique
            for (int i = 0; i < 11; i++) out[i] = candidate[i];
            return n;
        }
    }
    // Fallback (shouldn't happen in practice)
    format_83_name("UNKNO~1", 7, out);
    return 1;
}

// Directory slot: identifies a position within a cluster chain
struct dir_slot {
    uint32_t cluster;
    int      index;   // entry index within that cluster (0..entries_per_cluster-1)
};

// Write LFN entries for a given long name into dir slots before the 8.3 entry.
// lfn_count = ceil(name_len / 13)
// Slots are at [start_index - lfn_count + 1 .. start_index] (LFN) then [start_index + 1] (8.3)
// Actually slots are consecutive: LFN entries then 8.3 entry.
// The slots array provides the positions.
static void write_lfn_entries(uint32_t dir_cluster, const dir_slot *slots,
                               int lfn_count, const char *name, int name_len,
                               const char short_name[11]) {
    uint8_t chk = lfn_checksum((const uint8_t *)short_name);

    for (int n = 0; n < lfn_count; n++) {
        int seq = lfn_count - n; // reverse order: highest seq first
        bool is_last = (n == 0); // first LFN entry written is the last in sequence

        fat_dir_entry lfn_de;
        uint8_t *raw = (uint8_t *)&lfn_de;
        for (int k = 0; k < 32; k++) raw[k] = 0;
        raw[0] = seq | (is_last ? 0x40 : 0x00);
        raw[11] = 0x0F; // LFN attribute
        raw[12] = 0x00; // type
        raw[13] = chk;
        // cluster field at 26-27 = 0

        // Fill UCS-2 characters
        static const int offsets[] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
        int base = n * 13;

        for (int c = 0; c < 13; c++) {
            int pos = base + c;
            uint16_t ucs2;
            if (pos < name_len) {
                ucs2 = (uint16_t)(uint8_t)name[pos];
            } else if (pos == name_len) {
                ucs2 = 0x0000; // null terminator
            } else {
                ucs2 = 0xFFFF; // padding
            }
            raw[offsets[c]]     = ucs2 & 0xFF;
            raw[offsets[c] + 1] = (ucs2 >> 8) & 0xFF;
        }

        // Write this LFN entry at the slot
        write_dir_entry_at(dir_cluster, slots[n].index, &lfn_de);
    }
}

// ===================== Path resolution =====================

// Find a directory entry by name (supports LFN + 8.3 dual matching)
// name is pointer+length (zero-copy from resolve_path)
static bool find_dir_entry(uint32_t dir_cluster, const char *name,
                           int name_len, fat_dir_entry *out) {
    static char lfn_buf[256]; // static: single thread, no recursion
    lfn_buf[0] = '\0';

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        int slot = read_cluster(cluster);
        if (slot < 0) return false;

        uint8_t *data = cache[slot].data;
        int entries = bytes_per_cluster / 32;
        for (int i = 0; i < entries; i++) {
            fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
            if (de->name[0] == 0x00) return false;
            if (de->name[0] == 0xE5) { lfn_buf[0] = '\0'; continue; }
            if (de->attr == 0x0F) {
                collect_lfn_entry(de, lfn_buf);
                continue;
            }

            // Short name entry: check LFN match then 8.3 match
            bool matched = false;
            if (lfn_buf[0] != '\0') {
                matched = match_lfn_name(lfn_buf, name, name_len);
            }
            if (!matched) {
                matched = match_83_name(de->name, name, name_len);
            }

            if (matched) {
                *out = *de;
                return true;
            }
            lfn_buf[0] = '\0'; // reset LFN buffer for next entry
        }
        cluster = fat_read_entry(cluster);
    }
    return false;
}

// Resolve absolute path (zero-copy path parsing)
static bool resolve_path(const char *path, fat_dir_entry *out,
                         uint32_t *out_cluster) {
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
        const char *comp_start = p;
        int comp_len = 0;
        while (*p && *p != '/') { p++; comp_len++; }
        if (*p == '/') p++;

        fat_dir_entry de;
        if (!find_dir_entry(dir_cluster, comp_start, comp_len, &de))
            return false;

        if (*p == '\0') {
            *out = de;
            *out_cluster = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
            if (*out_cluster == 0) *out_cluster = root_cluster;
            return true;
        }

        if (!(de.attr & 0x10)) return false;
        dir_cluster = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
        if (dir_cluster == 0) dir_cluster = root_cluster;
    }
    return false;
}

// Resolve parent directory path, returning parent cluster and leaf name.
static bool resolve_parent(const char *path, uint32_t *parent_cluster,
                           char *leaf_name, int leaf_buf_size) {
    if (path[0] != '/') return false;

    int last_slash = -1;
    int path_len = 0;
    while (path[path_len]) {
        if (path[path_len] == '/') last_slash = path_len;
        path_len++;
    }

    if (path_len == 1) return false;

    const char *leaf_start = path + last_slash + 1;
    int leaf_len = 0;
    while (leaf_start[leaf_len] && leaf_len < leaf_buf_size - 1) {
        leaf_name[leaf_len] = leaf_start[leaf_len];
        leaf_len++;
    }
    leaf_name[leaf_len] = '\0';

    if (last_slash == 0) {
        *parent_cluster = root_cluster;
        return true;
    }

    char parent_path[256];
    for (int i = 0; i < last_slash; i++) parent_path[i] = path[i];
    parent_path[last_slash] = '\0';

    fat_dir_entry parent_de;
    uint32_t pcluster;
    if (!resolve_path(parent_path, &parent_de, &pcluster)) return false;
    if (!(parent_de.attr & 0x10)) return false;

    *parent_cluster = pcluster;
    return true;
}

// Directory slot: identifies a position within a cluster chain
// Find N consecutive free 32-byte slots in a directory cluster chain.
// If not enough room in current cluster, skip to next cluster.
// Only extends chain when all existing clusters are full.
static bool find_free_dir_slots(uint32_t dir_cluster, int needed, dir_slot *slots_out) {
    uint32_t cluster = dir_cluster;
    int entries_per_cluster = bytes_per_cluster / 32;
    int found = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        int cache_slot = read_cluster(cluster);
        if (cache_slot < 0) return false;

        uint8_t *data = cache[cache_slot].data;
        int run_start = -1;
        int run_len = 0;

        for (int i = 0; i < entries_per_cluster; i++) {
            fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
            bool is_free = (de->name[0] == 0x00 || de->name[0] == 0xE5);

            if (is_free) {
                if (run_len == 0) run_start = i;
                run_len++;
                if (run_len >= needed) {
                    // Found enough consecutive slots in this cluster
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

        uint32_t next = fat_read_entry(cluster);
        if (next >= 0x0FFFFFF8) {
            // End of chain — extend
            uint32_t new_cluster;
            int rc = dir_chain_extend(dir_cluster, &new_cluster);
            if (rc != 0) return false;
            // New cluster is all zeros, so index 0..needed-1 are free
            for (int j = 0; j < needed; j++) {
                slots_out[j].cluster = new_cluster;
                slots_out[j].index = j;
            }
            return true;
        }
        cluster = next;
    }
    return false;
}

// Legacy single-slot finder (for mkdir's . and .. entries)
static bool find_free_dir_slot(uint32_t dir_cluster, dir_slot *slot_out) {
    return find_free_dir_slots(dir_cluster, 1, slot_out);
}

// Write a directory entry at a specific slot position — only writes the target sector
static int write_dir_entry_at(uint32_t cluster, int index, const fat_dir_entry *entry) {
    int cache_slot = read_cluster(cluster);
    if (cache_slot < 0) return 1;

    uint8_t *data = cache[cache_slot].data;
    __memcpy(data + index * 32, entry, 32);

    // Write only the sector containing this entry
    int entry_offset = index * 32;
    int sector_in_cluster = entry_offset / 512;
    uint32_t cluster_lba = data_start_lba + (cluster - 2) * sectors_per_cluster;

    if (disk_write(cluster_lba + sector_in_cluster, 1, data + sector_in_cluster * 512) != 0)
        return 2;

    // If entry spans sector boundary (last 32 bytes of a sector), write next sector too
    int offset_in_sector = entry_offset % 512;
    if (offset_in_sector + 32 > 512 && sector_in_cluster + 1 < (int)sectors_per_cluster) {
        if (disk_write(cluster_lba + sector_in_cluster + 1, 1,
                       data + (sector_in_cluster + 1) * 512) != 0)
            return 2;
    }

    return 0;
}

// ===================== Open file table =====================
#define MAX_OPEN_FILES 8

struct open_file {
    bool     used;
    uint32_t start_cluster;
    uint32_t file_size;
};

static open_file open_table[MAX_OPEN_FILES];

static int alloc_fd() {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_table[i].used) return i;
    }
    return -1;
}

// ===================== FAT32 init =====================
static void fat32_init() {
    disk_read(0, 1);
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

    disk_read(part_start_lba, 1);
    uint8_t *bpb = (uint8_t *)dresp->data;

    uint16_t bps = (uint16_t)bpb[11] | ((uint16_t)bpb[12] << 8);
    sectors_per_cluster = bpb[13];
    uint16_t reserved = (uint16_t)bpb[14] | ((uint16_t)bpb[15] << 8);
    spf32 = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
            ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    root_cluster = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                   ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);

    if (bps != 512 || sectors_per_cluster == 0 || spf32 == 0 || root_cluster < 2) {
        const char msg[] = "EBPB\n";
        serial_write(msg, sizeof(msg) - 1);
        while (1) { struct recv_msg m; recv(&m, 0); }
    }

    fat_start_lba = part_start_lba + reserved;
    data_start_lba = fat_start_lba + spf32 * 2;
    bytes_per_cluster = sectors_per_cluster * 512;

    uint32_t data_sectors = part_total_sectors - (data_start_lba - part_start_lba);
    total_data_clusters = data_sectors / sectors_per_cluster;

    // Pre-populate FAT cache by reading all FAT sectors
    for (uint32_t s = 0; s < spf32 && s < FAT_CACHE_PAGES; s++) {
        fat_cache_read(fat_start_lba + s);
    }
}

// ===================== Timestamp =====================
#define HARD_DATE 0x5A21
#define HARD_TIME 0x0000

// ===================== Command handlers =====================

static void handle_readdir(const char *path) {
    fat_dir_entry de;
    uint32_t dir_cluster;
    if (!resolve_path(path, &de, &dir_cluster)) {
        fresp->status = 1;
        return;
    }
    if (!(de.attr & 0x10)) {
        fresp->status = 2;
        return;
    }

    uint32_t offset = freq->offset;
    uint32_t count = freq->count;
    if (count == 0) count = 30; // default page size

    fs_dirent *out = (fs_dirent *)fresp->data;
    uint32_t max_entries = 8176 / sizeof(fs_dirent); // 30 with 272-byte dirent
    if (count > max_entries) count = max_entries;

    static char lfn_buf[256];
    lfn_buf[0] = '\0';

    uint32_t cluster = dir_cluster;
    uint32_t entry_count = 0;   // total valid entries found
    uint32_t out_count = 0;     // entries written to output

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        int slot = read_cluster(cluster);
        if (slot < 0) { fresp->status = 3; return; }

        uint8_t *data = cache[slot].data;
        int entries = bytes_per_cluster / 32;
        for (int i = 0; i < entries; i++) {
            fat_dir_entry *fde = (fat_dir_entry *)(data + i * 32);
            if (fde->name[0] == 0x00) goto done;
            if (fde->name[0] == 0xE5) { lfn_buf[0] = '\0'; continue; }
            if (fde->attr == 0x0F) {
                collect_lfn_entry(fde, lfn_buf);
                continue;
            }
            if (fde->name[0] == '.') {
                lfn_buf[0] = '\0';
                continue;
            }

            // This is a short name entry (the authoritative entry)
            if (entry_count >= offset) {
                // Write to output
                fs_dirent *d = &out[out_count];
                if (lfn_buf[0] != '\0') {
                    // Use LFN name
                    int j = 0;
                    while (lfn_buf[j] && j < 255) {
                        d->name[j] = lfn_buf[j];
                        j++;
                    }
                    d->name[j] = '\0';
                } else {
                    // Format 8.3 as readable name
                    convert_83_to_name(fde->name, fde->nt_res, d->name, 256);
                }
                d->size = fde->file_size;
                d->date = fde->wrt_date;
                d->time = fde->wrt_time;
                d->attr = fde->attr;
                out_count++;
                if (out_count >= count) goto done;
            }
            entry_count++;
            lfn_buf[0] = '\0';
        }
        cluster = fat_read_entry(cluster);
    }
done:
    fresp->status = 0;
    fresp->total = out_count;
}

static void handle_open(const char *path) {
    fat_dir_entry de;
    uint32_t cluster;
    if (!resolve_path(path, &de, &cluster)) {
        fresp->status = 1;
        return;
    }
    if (de.attr & 0x10) {
        fresp->status = 2;
        return;
    }

    int fd = alloc_fd();
    if (fd < 0) {
        fresp->status = 5;
        return;
    }

    open_table[fd].used = true;
    open_table[fd].start_cluster = cluster;
    open_table[fd].file_size = de.file_size;

    fresp->status = 0;
    fresp->fd = (uint32_t)fd;
    fresp->total = de.file_size;
}

static void handle_read(uint32_t fd, uint32_t offset, uint32_t count) {
    if (fd >= MAX_OPEN_FILES || !open_table[fd].used) {
        fresp->status = 1;
        return;
    }

    open_file *f = &open_table[fd];
    if (offset >= f->file_size) {
        fresp->status = 0;
        fresp->count = 0;
        return;
    }

    uint32_t avail = f->file_size - offset;
    if (count > avail) count = avail;
    if (count > 8176) count = 8176;

    uint32_t cluster_offset = offset / bytes_per_cluster;
    uint32_t in_cluster_offset = offset % bytes_per_cluster;

    uint32_t cluster = f->start_cluster;
    for (uint32_t i = 0; i < cluster_offset; i++) {
        cluster = fat_read_entry(cluster);
        if (cluster >= 0x0FFFFFF8) {
            fresp->status = 2;
            return;
        }
    }

    uint32_t bytes_read = 0;
    uint8_t *dst = (uint8_t *)fresp->data;

    while (bytes_read < count && cluster >= 2 && cluster < 0x0FFFFFF8) {
        int slot = read_cluster(cluster);
        if (slot < 0) { fresp->status = 3; return; }

        uint32_t to_copy = bytes_per_cluster - in_cluster_offset;
        if (to_copy > count - bytes_read) to_copy = count - bytes_read;

        __memcpy(dst + bytes_read, cache[slot].data + in_cluster_offset, to_copy);
        bytes_read += to_copy;
        in_cluster_offset = 0;
        cluster = fat_read_entry(cluster);
    }

    fresp->status = 0;
    fresp->count = bytes_read;
}

static void handle_close(uint32_t fd) {
    if (fd < MAX_OPEN_FILES) {
        open_table[fd].used = false;
    }
    fresp->status = 0;
}

static void handle_raw_read(uint32_t lba, uint32_t count) {
    uint32_t max_sectors = 8176 / 512;
    if (count > max_sectors) count = max_sectors;
    if (count == 0) count = 1;

    if (disk_read(lba, count) != 0) {
        fresp->status = dresp->status;
        return;
    }

    uint32_t bytes = count * 512;
    __memcpy((void *)fresp->data, (const void *)dresp->data, bytes);
    fresp->status = 0;
    fresp->count = bytes;
}

// ===================== Write command handlers =====================

// touch: create empty file (with LFN) or update timestamp if exists
static void handle_create(const char *path) {
    // Check if file already exists (using dual LFN+8.3 matching)
    fat_dir_entry existing;
    uint32_t existing_cluster;
    if (resolve_path(path, &existing, &existing_cluster)) {
        // File/directory exists — update timestamp only
        uint32_t parent_cluster;
        char leaf_name[256];
        if (!resolve_parent(path, &parent_cluster, leaf_name, 256)) {
            fresp->status = 1;
            return;
        }

        int leaf_len = 0;
        while (leaf_name[leaf_len]) leaf_len++;

        uint32_t cluster = parent_cluster;
        static char lfn_buf[256];
        lfn_buf[0] = '\0';

        while (cluster >= 2 && cluster < 0x0FFFFFF8) {
            int cache_slot = read_cluster(cluster);
            if (cache_slot < 0) { fresp->status = 1; return; }

            uint8_t *data = cache[cache_slot].data;
            int entries = bytes_per_cluster / 32;
            for (int i = 0; i < entries; i++) {
                fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
                if (de->name[0] == 0x00) break;
                if (de->name[0] == 0xE5) { lfn_buf[0] = '\0'; continue; }
                if (de->attr == 0x0F) {
                    collect_lfn_entry(de, lfn_buf);
                    continue;
                }

                bool matched = false;
                if (lfn_buf[0] != '\0')
                    matched = match_lfn_name(lfn_buf, leaf_name, leaf_len);
                if (!matched)
                    matched = match_83_name(de->name, leaf_name, leaf_len);

                if (matched) {
                    de->wrt_date = HARD_DATE;
                    de->wrt_time = HARD_TIME;
                    de->lst_acc_date = HARD_DATE;

                    int entry_offset = i * 32;
                    int sector_in_cluster = entry_offset / 512;
                    uint32_t cluster_lba = data_start_lba + (cluster - 2) * sectors_per_cluster;
                    if (disk_write(cluster_lba + sector_in_cluster, 1,
                                   data + sector_in_cluster * 512) != 0) {
                        fresp->status = 1;
                        return;
                    }
                    fresp->status = 0;
                    return;
                }
                lfn_buf[0] = '\0';
            }
            cluster = fat_read_entry(cluster);
        }
        fresp->status = 1;
        return;
    }

    // File doesn't exist — create it
    uint32_t parent_cluster;
    char leaf_name[256];
    if (!resolve_parent(path, &parent_cluster, leaf_name, 256)) {
        fresp->status = 1;
        return;
    }

    int leaf_len = 0;
    while (leaf_name[leaf_len]) leaf_len++;

    // Generate 8.3 short name
    char short_name[11];
    generate_short_name(leaf_name, leaf_len, short_name, parent_cluster);

    // Calculate LFN entry count
    int lfn_count = (leaf_len + 12) / 13; // ceil(leaf_len / 13)
    int total_slots = lfn_count + 1; // LFN entries + 1 short name entry

    // Find consecutive free slots
    dir_slot slots[20]; // max 19 LFN + 1 short = 20
    if (!find_free_dir_slots(parent_cluster, total_slots, slots)) {
        fresp->status = 4;
        return;
    }

    // Write LFN entries (if any)
    if (lfn_count > 0) {
        write_lfn_entries(parent_cluster, slots, lfn_count, leaf_name, leaf_len, short_name);
    }

    // Create short name entry
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

    int rc = write_dir_entry_at(slots[lfn_count].cluster, slots[lfn_count].index, &new_entry);
    if (rc != 0) {
        fresp->status = rc;
        return;
    }

    fresp->status = 0;
}

// mkdir: create directory with . and .. entries (with LFN)
static void handle_mkdir(const char *path) {
    // Check if already exists (dual matching)
    fat_dir_entry existing;
    uint32_t existing_cluster;
    if (resolve_path(path, &existing, &existing_cluster)) {
        fresp->status = 3;
        return;
    }

    uint32_t parent_cluster;
    char leaf_name[256];
    if (!resolve_parent(path, &parent_cluster, leaf_name, 256)) {
        fresp->status = 1;
        return;
    }

    int leaf_len = 0;
    while (leaf_name[leaf_len]) leaf_len++;

    // Allocate cluster for new directory
    uint32_t new_dir_cluster = allocate_cluster();
    if (new_dir_cluster == 0) {
        fresp->status = 4;
        return;
    }

    // Initialize new directory with . and .. entries
    for (int i = 0; i < bytes_per_cluster; i++) dir_cluster_data_buf[i] = 0;

    fat_dir_entry dot_entry;
    dot_entry.name[0] = '.'; for (int i = 1; i < 11; i++) dot_entry.name[i] = ' ';
    dot_entry.attr = 0x10;
    dot_entry.nt_res = 0;
    dot_entry.crt_time_tenth = 0;
    dot_entry.crt_time = HARD_TIME;
    dot_entry.crt_date = HARD_DATE;
    dot_entry.lst_acc_date = HARD_DATE;
    dot_entry.fst_clus_hi = (new_dir_cluster >> 16) & 0xFFFF;
    dot_entry.wrt_time = HARD_TIME;
    dot_entry.wrt_date = HARD_DATE;
    dot_entry.fst_clus_lo = new_dir_cluster & 0xFFFF;
    dot_entry.file_size = 0;
    __memcpy(dir_cluster_data_buf, &dot_entry, 32);

    fat_dir_entry dotdot_entry;
    dotdot_entry.name[0] = '.'; dotdot_entry.name[1] = '.';
    for (int i = 2; i < 11; i++) dotdot_entry.name[i] = ' ';
    dotdot_entry.attr = 0x10;
    dotdot_entry.nt_res = 0;
    dotdot_entry.crt_time_tenth = 0;
    dotdot_entry.crt_time = HARD_TIME;
    dotdot_entry.crt_date = HARD_DATE;
    dotdot_entry.lst_acc_date = HARD_DATE;
    dotdot_entry.fst_clus_hi = (parent_cluster >> 16) & 0xFFFF;
    dotdot_entry.wrt_time = HARD_TIME;
    dotdot_entry.wrt_date = HARD_DATE;
    dotdot_entry.fst_clus_lo = parent_cluster & 0xFFFF;
    dotdot_entry.file_size = 0;
    __memcpy(dir_cluster_data_buf + 32, &dotdot_entry, 32);

    // Write new directory cluster to disk
    uint32_t new_dir_lba = data_start_lba + (new_dir_cluster - 2) * sectors_per_cluster;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (disk_write(new_dir_lba + s, 1, dir_cluster_data_buf + s * 512) != 0) {
            fresp->status = 5;
            return;
        }
    }

    // Generate short name and find slots in parent
    char short_name[11];
    generate_short_name(leaf_name, leaf_len, short_name, parent_cluster);

    int lfn_count = (leaf_len + 12) / 13;
    int total_slots = lfn_count + 1;

    dir_slot slots[20];
    if (!find_free_dir_slots(parent_cluster, total_slots, slots)) {
        fresp->status = 4;
        return;
    }

    // Write LFN entries
    if (lfn_count > 0) {
        write_lfn_entries(parent_cluster, slots, lfn_count, leaf_name, leaf_len, short_name);
    }

    // Create short name entry in parent
    fat_dir_entry new_entry;
    __memcpy(new_entry.name, short_name, 11);
    new_entry.attr = 0x10;
    new_entry.nt_res = 0;
    new_entry.crt_time_tenth = 0;
    new_entry.crt_time = HARD_TIME;
    new_entry.crt_date = HARD_DATE;
    new_entry.lst_acc_date = HARD_DATE;
    new_entry.fst_clus_hi = (new_dir_cluster >> 16) & 0xFFFF;
    new_entry.wrt_time = HARD_TIME;
    new_entry.wrt_date = HARD_DATE;
    new_entry.fst_clus_lo = new_dir_cluster & 0xFFFF;
    new_entry.file_size = 0;

    int rc = write_dir_entry_at(slots[lfn_count].cluster, slots[lfn_count].index, &new_entry);
    if (rc != 0) {
        fresp->status = rc;
        return;
    }

    fresp->status = 0;
}

// ===================== Main loop =====================
extern "C" void _start() {
    serial_write("fs_driver: _start\n", 18);
    void *fs_shm_ptr = NULL;
    shm_create(4 * 4096, &fs_shm_ptr);
    uint64_t fs_shm = (uint64_t)fs_shm_ptr;
    fs_hdr = (volatile fs_shm_header *)(fs_shm + FS_SHM_HEADER_OFFSET);
    freq   = (volatile fs_req_shm *)(fs_shm + FS_REQ_OFFSET);
    fresp  = (volatile fs_resp_shm *)(fs_shm + FS_RESP_OFFSET);

    serial_write("fs_driver: attaching disk_shm\n", 29);
    while ((disk_driver_pid = device_lookup(DEV_DISK)) < 0) {
        struct recv_msg m;
        recv(&m, 1);
    }
    void *disk_shm_ptr = NULL;
    while (shm_attach(disk_driver_pid, &disk_shm_ptr) < 0) {
        struct recv_msg m;
        recv(&m, 1);
    }
    uint64_t disk_shm = (uint64_t)disk_shm_ptr;
    serial_write("fs_driver: disk_shm attached\n", 29);
    disk_hdr = (volatile disk_shm_header *)(disk_shm + DISK_SHM_HEADER_OFFSET);
    dreq     = (volatile disk_req_shm *)(disk_shm + DISK_REQ_OFFSET);
    dresp    = (volatile disk_resp_shm *)(disk_shm + DISK_RESP_OFFSET);
    disk_hdr->fs_driver_pid = getpid();

    // Initialize caches
    for (int i = 0; i < CACHE_SLOTS; i++) {
        cache[i].cluster = 0xFFFFFFFF;
        cache[i].age = 0;
    }
    for (int i = 0; i < FAT_CACHE_PAGES; i++) {
        fat_cache[i].sector_lba = 0xFFFFFFFF;
        fat_cache_age[i] = 0;
    }

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        open_table[i].used = false;
    }

    fat32_init();
    serial_write("fs_driver: fat32_init done\n", 26);

    device_register(getpid(), DEV_FS);
    serial_write("fs_driver: registered\n", 22);

    while (1) {
        struct recv_msg m;
        recv(&m, 0);

        if (m.type != RECV_RPC) continue;

        uint32_t cmd = freq->cmd;

        fresp->status = 0;
        fresp->fd = 0;
        fresp->count = 0;
        fresp->total = 0;

        switch (cmd) {
        case FS_CMD_READDIR: {
            char path[256];
            for (int i = 0; i < 256; i++) path[i] = freq->path[i];
            handle_readdir(path);
            break;
        }
        case FS_CMD_OPEN: {
            char path[256];
            for (int i = 0; i < 256; i++) path[i] = freq->path[i];
            handle_open(path);
            break;
        }
        case FS_CMD_READ:
            handle_read(freq->fd, freq->offset, freq->count);
            break;
        case FS_CMD_CLOSE:
            handle_close(freq->fd);
            break;
        case FS_CMD_RAW_READ:
            handle_raw_read(freq->lba, freq->count);
            break;
        case FS_CMD_CREATE: {
            char path[256];
            for (int i = 0; i < 256; i++) path[i] = freq->path[i];
            handle_create(path);
            break;
        }
        case FS_CMD_MKDIR: {
            char path[256];
            for (int i = 0; i < 256; i++) path[i] = freq->path[i];
            handle_mkdir(path);
            break;
        }
        default:
            fresp->status = 0xFF;
            break;
        }

        fs_rpc_reply reply;
        reply.status = fresp->status;
        reply.fd     = fresp->fd;
        reply.count  = fresp->count;
        reply.total  = fresp->total;
        for (int i = 0; i < 48; i++) reply.reserved[i] = 0;
        rpc_reply(&reply);
    }
}
