// ===================== FAT32 filesystem driver (user-space) — read + write + LFN
// Receives requests via sys_msg (variable-length IPC), performs FAT32 operations,
// replies via sys_msg_resp. Accesses disk via disk_driver through
// DISK_REQ/DISK_RESP shared pages.
#include <stdint.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/device.h>
#include <sys/serial.h>
#include "common/shm.h"
#include "common/dev.h"
#include "common/errno.h"

// Static buffers to avoid stack overflow (4KB arrays on stack exceed user stack page)
static uint8_t dir_cluster_data_buf[4096];   // used in handle_mkdir
static uint8_t zero_cluster_buf[4096];       // used in dir_chain_extend
static uint8_t sector_buf[512];              // used in fat_write_entry

// ===================== Shared page pointers (disk SHM only) =====================
static volatile disk_req_shm  *dreq;
static volatile disk_resp_shm *dresp;
static volatile disk_shm_header *disk_hdr;

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
        recv(&m, NULL, 0, 0);
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

    int slot = fat_cache_read(fat_sector);
    if (slot < 0) return 1;

    __memcpy(sector_buf, fat_cache[slot].data, 512);

    uint8_t *p = sector_buf + offset_in_sector;
    uint32_t old_val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t new_val = (old_val & 0xF0000000) | (value & 0x0FFFFFFF);
    p[0] = new_val & 0xFF;
    p[1] = (new_val >> 8) & 0xFF;
    p[2] = (new_val >> 16) & 0xFF;
    p[3] = (new_val >> 24) & 0xFF;

    if (disk_write(fat_sector, 1, sector_buf) != 0) return 2;

    uint32_t fat2_sector = fat_sector + spf32;
    if (disk_write(fat2_sector, 1, sector_buf) != 0) return 3;

    __memcpy(fat_cache[slot].data, sector_buf, 512);

    return 0;
}

// ===================== Cluster allocation =====================
static uint32_t next_free_hint = 2;

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

// Forward declarations
static bool find_dir_entry(uint32_t dir_cluster, const char *name,
                           int name_len, fat_dir_entry *out);
static int write_dir_entry_at(uint32_t cluster, int index, const fat_dir_entry *entry);

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
    if (dot_pos < 0) {
        base_len = name_len;
        ext_len = 0;
    } else {
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

        int tilde_len = (n >= 10) ? 3 : 2;
        int base_chars = 8 - tilde_len;
        if (base_chars > 6) base_chars = 6;

        int pos = 0;
        for (int i = 0; i < base_chars && basename[i]; i++)
            candidate[pos++] = basename[i];
        candidate[pos++] = '~';
        if (n >= 10) {
            candidate[pos++] = '0' + (n / 10);
            candidate[pos++] = '0' + (n % 10);
        } else {
            candidate[pos++] = '0' + n;
        }
        for (int i = 0; i < 3 && extname[i]; i++)
            candidate[8 + i] = extname[i];

        fat_dir_entry dummy;
        if (!find_dir_entry(dir_cluster, candidate, 11, &dummy)) {
            for (int i = 0; i < 11; i++) out[i] = candidate[i];
            return n;
        }
    }
    format_83_name("UNKNO~1", 7, out);
    return 1;
}

struct dir_slot {
    uint32_t cluster;
    int      index;
};

static void write_lfn_entries(uint32_t dir_cluster, const dir_slot *slots,
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
        raw[11] = 0x0F;
        raw[12] = 0x00;
        raw[13] = chk;

        static const int offsets[] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
        int base = n * 13;

        for (int c = 0; c < 13; c++) {
            int pos = base + c;
            uint16_t ucs2;
            if (pos < name_len) {
                ucs2 = (uint16_t)(uint8_t)name[pos];
            } else if (pos == name_len) {
                ucs2 = 0x0000;
            } else {
                ucs2 = 0xFFFF;
            }
            raw[offsets[c]]     = ucs2 & 0xFF;
            raw[offsets[c] + 1] = (ucs2 >> 8) & 0xFF;
        }

        write_dir_entry_at(dir_cluster, slots[n].index, &lfn_de);
    }
}

// ===================== Path resolution =====================

static bool find_dir_entry(uint32_t dir_cluster, const char *name,
                           int name_len, fat_dir_entry *out) {
    static char lfn_buf[256];
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
            lfn_buf[0] = '\0';
        }
        cluster = fat_read_entry(cluster);
    }
    return false;
}

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

static bool find_free_dir_slots(uint32_t dir_cluster, int needed, dir_slot *slots_out) {
    uint32_t cluster = dir_cluster;
    int entries_per_cluster = bytes_per_cluster / 32;

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
            uint32_t new_cluster;
            int rc = dir_chain_extend(dir_cluster, &new_cluster);
            if (rc != 0) return false;
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

static bool find_free_dir_slot(uint32_t dir_cluster, dir_slot *slot_out) {
    return find_free_dir_slots(dir_cluster, 1, slot_out);
}

static int write_dir_entry_at(uint32_t cluster, int index, const fat_dir_entry *entry) {
    int cache_slot = read_cluster(cluster);
    if (cache_slot < 0) return 1;

    uint8_t *data = cache[cache_slot].data;
    __memcpy(data + index * 32, entry, 32);

    int entry_offset = index * 32;
    int sector_in_cluster = entry_offset / 512;
    uint32_t cluster_lba = data_start_lba + (cluster - 2) * sectors_per_cluster;

    if (disk_write(cluster_lba + sector_in_cluster, 1, data + sector_in_cluster * 512) != 0)
        return 2;

    int offset_in_sector = entry_offset % 512;
    if (offset_in_sector + 32 > 512 && sector_in_cluster + 1 < (int)sectors_per_cluster) {
        if (disk_write(cluster_lba + sector_in_cluster + 1, 1,
                       data + (sector_in_cluster + 1) * 512) != 0)
            return 2;
    }

    return 0;
}

// ===================== Multi-client session management =====================
#define MAX_CLIENTS  16
#define MAX_SESSION_FDS 8

struct session_open_file {
    bool     used;
    uint32_t start_cluster;
    uint32_t file_size;
};

struct client_session {
    pid_t client_pid;  // -1 = free slot
    struct session_open_file open_files[MAX_SESSION_FDS];
};

static struct client_session sessions[MAX_CLIENTS];

static struct client_session *get_session(pid_t pid) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].client_pid == pid) return &sessions[i];
    }
    // Allocate new session
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

// ===================== Message protocol (shared with libc) =====================
#define FILE_CMD_OPEN      1
#define FILE_CMD_READ      2
#define FILE_CMD_WRITE     3
#define FILE_CMD_CLOSE     4
#define FILE_CMD_READDIR   5
#define FILE_CMD_CREATE    6
#define FILE_CMD_MKDIR     7
#define FILE_CMD_RAW_READ  8

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

// Maximum reply buffer (header + up to ~64KB data)
// We use a static buffer to avoid large stack allocations
#define REPLY_BUF_SIZE (sizeof(struct file_resp) + 65536)
static uint8_t reply_buf[REPLY_BUF_SIZE];

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
        while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    }

    fat_start_lba = part_start_lba + reserved;
    data_start_lba = fat_start_lba + spf32 * 2;
    bytes_per_cluster = sectors_per_cluster * 512;

    uint32_t data_sectors = part_total_sectors - (data_start_lba - part_start_lba);
    total_data_clusters = data_sectors / sectors_per_cluster;

    for (uint32_t s = 0; s < spf32 && s < FAT_CACHE_PAGES; s++) {
        fat_cache_read(fat_start_lba + s);
    }
}

// ===================== Timestamp =====================
#define HARD_DATE 0x5A21
#define HARD_TIME 0x0000

// ===================== Command handlers (write to reply_buf) =====================

// Maximum data bytes in a single reply (leaving room for file_resp header)
#define MAX_REPLY_DATA  (65536)

static void handle_open(struct client_session *sess, const char *path,
                         uint32_t flags, struct file_resp *resp) {
    fat_dir_entry de;
    uint32_t cluster;
    if (!resolve_path(path, &de, &cluster)) {
        resp->status = -ENOENT;
        return;
    }
    if (de.attr & 0x10) {
        resp->status = -EISDIR;
        return;
    }

    int fd = session_alloc_fd(sess);
    if (fd < 0) {
        resp->status = -ENOMEM;
        return;
    }

    sess->open_files[fd].used = true;
    sess->open_files[fd].start_cluster = cluster;
    sess->open_files[fd].file_size = de.file_size;

    resp->status = 0;
    resp->fd = (uint32_t)fd;
    resp->file_size = de.file_size;
}

static void handle_read(struct client_session *sess, uint32_t fs_fd,
                         uint64_t offset, uint32_t count, struct file_resp *resp) {
    if (fs_fd >= MAX_SESSION_FDS || !sess->open_files[fs_fd].used) {
        resp->status = -EBADF;
        return;
    }

    session_open_file *f = &sess->open_files[fs_fd];
    if (offset >= f->file_size) {
        resp->status = 0;
        resp->count = 0;
        return;
    }

    uint32_t avail = f->file_size - (uint32_t)offset;
    if (count > avail) count = avail;
    if (count > MAX_REPLY_DATA) count = MAX_REPLY_DATA;

    uint32_t cluster_offset = (uint32_t)offset / bytes_per_cluster;
    uint32_t in_cluster_offset = (uint32_t)offset % bytes_per_cluster;

    uint32_t cluster = f->start_cluster;
    for (uint32_t i = 0; i < cluster_offset; i++) {
        cluster = fat_read_entry(cluster);
        if (cluster >= 0x0FFFFFF8) {
            resp->status = -EINVAL;
            return;
        }
    }

    uint32_t bytes_read = 0;
    uint8_t *dst = resp->data;

    while (bytes_read < count && cluster >= 2 && cluster < 0x0FFFFFF8) {
        int slot = read_cluster(cluster);
        if (slot < 0) { resp->status = -EIO; return; }

        uint32_t to_copy = bytes_per_cluster - in_cluster_offset;
        if (to_copy > count - bytes_read) to_copy = count - bytes_read;

        __memcpy(dst + bytes_read, cache[slot].data + in_cluster_offset, to_copy);
        bytes_read += to_copy;
        in_cluster_offset = 0;
        cluster = fat_read_entry(cluster);
    }

    resp->status = 0;
    resp->count = bytes_read;
}

static void handle_close(struct client_session *sess, uint32_t fs_fd,
                          struct file_resp *resp) {
    if (fs_fd < MAX_SESSION_FDS) {
        sess->open_files[fs_fd].used = false;
    }
    resp->status = 0;
}

static void handle_readdir(const char *path, uint32_t rd_offset,
                            uint32_t rd_count, struct file_resp *resp) {
    fat_dir_entry de;
    uint32_t dir_cluster;
    if (!resolve_path(path, &de, &dir_cluster)) {
        resp->status = -ENOENT;
        return;
    }
    if (!(de.attr & 0x10)) {
        resp->status = -ENOTDIR;
        return;
    }

    if (rd_count == 0) rd_count = 30;

    fs_dirent *out = (fs_dirent *)resp->data;
    uint32_t max_entries = MAX_REPLY_DATA / sizeof(fs_dirent);
    if (rd_count > max_entries) rd_count = max_entries;

    static char lfn_buf[256];
    lfn_buf[0] = '\0';

    uint32_t cluster = dir_cluster;
    uint32_t entry_count = 0;
    uint32_t out_count = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        int slot = read_cluster(cluster);
        if (slot < 0) { resp->status = -EIO; return; }

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

            if (entry_count >= rd_offset) {
                fs_dirent *d = &out[out_count];
                if (lfn_buf[0] != '\0') {
                    int j = 0;
                    while (lfn_buf[j] && j < 255) {
                        d->name[j] = lfn_buf[j];
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
                out_count++;
                if (out_count >= rd_count) goto done;
            }
            entry_count++;
            lfn_buf[0] = '\0';
        }
        cluster = fat_read_entry(cluster);
    }
done:
    resp->status = 0;
    resp->total = out_count;
    resp->count = out_count * sizeof(fs_dirent);
}

static void handle_raw_read(uint32_t lba, uint32_t count,
                             struct file_resp *resp) {
    uint32_t max_sectors = MAX_REPLY_DATA / 512;
    if (count > max_sectors) count = max_sectors;
    if (count == 0) count = 1;

    if (disk_read(lba, count) != 0) {
        resp->status = -EIO;
        return;
    }

    uint32_t bytes = count * 512;
    __memcpy(resp->data, (const void *)dresp->data, bytes);
    resp->status = 0;
    resp->count = bytes;
}

static void handle_create(const char *path, struct file_resp *resp) {
    fat_dir_entry existing;
    uint32_t existing_cluster;
    if (resolve_path(path, &existing, &existing_cluster)) {
        // File/directory exists — update timestamp only
        uint32_t parent_cluster;
        char leaf_name[256];
        if (!resolve_parent(path, &parent_cluster, leaf_name, 256)) {
            resp->status = -ENOENT;
            return;
        }

        int leaf_len = 0;
        while (leaf_name[leaf_len]) leaf_len++;

        uint32_t cluster = parent_cluster;
        static char lfn_buf[256];
        lfn_buf[0] = '\0';

        while (cluster >= 2 && cluster < 0x0FFFFFF8) {
            int cache_slot = read_cluster(cluster);
            if (cache_slot < 0) { resp->status = -EIO; return; }

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
                        resp->status = -EIO;
                        return;
                    }
                    resp->status = 0;
                    return;
                }
                lfn_buf[0] = '\0';
            }
            cluster = fat_read_entry(cluster);
        }
        resp->status = -ENOENT;
        return;
    }

    // File doesn't exist — create it
    uint32_t parent_cluster;
    char leaf_name[256];
    if (!resolve_parent(path, &parent_cluster, leaf_name, 256)) {
        resp->status = -ENOENT;
        return;
    }

    int leaf_len = 0;
    while (leaf_name[leaf_len]) leaf_len++;

    char short_name[11];
    generate_short_name(leaf_name, leaf_len, short_name, parent_cluster);

    int lfn_count = (leaf_len + 12) / 13;
    int total_slots = lfn_count + 1;

    dir_slot slots[20];
    if (!find_free_dir_slots(parent_cluster, total_slots, slots)) {
        resp->status = -ENOMEM;
        return;
    }

    if (lfn_count > 0) {
        write_lfn_entries(parent_cluster, slots, lfn_count, leaf_name, leaf_len, short_name);
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

    int rc = write_dir_entry_at(slots[lfn_count].cluster, slots[lfn_count].index, &new_entry);
    if (rc != 0) {
        resp->status = -rc;
        return;
    }

    resp->status = 0;
}

static void handle_mkdir(const char *path, struct file_resp *resp) {
    fat_dir_entry existing;
    uint32_t existing_cluster;
    if (resolve_path(path, &existing, &existing_cluster)) {
        resp->status = -EEXIST;
        return;
    }

    uint32_t parent_cluster;
    char leaf_name[256];
    if (!resolve_parent(path, &parent_cluster, leaf_name, 256)) {
        resp->status = -ENOENT;
        return;
    }

    int leaf_len = 0;
    while (leaf_name[leaf_len]) leaf_len++;

    uint32_t new_dir_cluster = allocate_cluster();
    if (new_dir_cluster == 0) {
        resp->status = -ENOMEM;
        return;
    }

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

    uint32_t new_dir_lba = data_start_lba + (new_dir_cluster - 2) * sectors_per_cluster;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (disk_write(new_dir_lba + s, 1, dir_cluster_data_buf + s * 512) != 0) {
            resp->status = -EIO;
            return;
        }
    }

    char short_name[11];
    generate_short_name(leaf_name, leaf_len, short_name, parent_cluster);

    int lfn_count = (leaf_len + 12) / 13;
    int total_slots = lfn_count + 1;

    dir_slot slots[20];
    if (!find_free_dir_slots(parent_cluster, total_slots, slots)) {
        resp->status = -ENOMEM;
        return;
    }

    if (lfn_count > 0) {
        write_lfn_entries(parent_cluster, slots, lfn_count, leaf_name, leaf_len, short_name);
    }

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
        resp->status = -rc;
        return;
    }

    resp->status = 0;
}

// ===================== Main loop =====================
extern "C" void _start() {
    serial_write("fs_driver: _start\n", 18);

    serial_write("fs_driver: attaching disk_shm\n", 29);
    while ((disk_driver_pid = device_lookup(DEV_DISK)) < 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 1);
    }
    void *disk_shm_ptr = NULL;
    while (shm_attach(disk_driver_pid, &disk_shm_ptr) < 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 1);
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

    // Initialize sessions
    for (int i = 0; i < MAX_CLIENTS; i++) {
        sessions[i].client_pid = -1;
    }

    fat32_init();
    serial_write("fs_driver: fat32_init done\n", 26);

    device_register(getpid(), DEV_FS);
    serial_write("fs_driver: registered\n", 22);

    while (1) {
        struct recv_msg m;
        uint8_t data_buf[sizeof(struct file_req)];
        recv(&m, data_buf, sizeof(data_buf), 0);

        if (m.type != RECV_MSG) continue;

        pid_t client_pid = (pid_t)m.src;
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

        struct file_req *freq = (struct file_req *)data_buf;
        struct file_resp *resp = (struct file_resp *)reply_buf;
        resp->status = 0;
        resp->fd = 0;
        resp->file_size = 0;
        resp->count = 0;
        resp->total = 0;

        switch (freq->cmd) {
        case FILE_CMD_OPEN:
            handle_open(sess, freq->path, freq->flags, resp);
            break;
        case FILE_CMD_READ:
            handle_read(sess, freq->fs_fd, freq->offset, freq->count, resp);
            break;
        case FILE_CMD_WRITE:
            resp->status = -ENOSYS;
            break;
        case FILE_CMD_CLOSE:
            handle_close(sess, freq->fs_fd, resp);
            break;
        case FILE_CMD_READDIR:
            handle_readdir(freq->path, freq->readdir_offset, freq->readdir_count, resp);
            break;
        case FILE_CMD_CREATE:
            handle_create(freq->path, resp);
            break;
        case FILE_CMD_MKDIR:
            handle_mkdir(freq->path, resp);
            break;
        case FILE_CMD_RAW_READ:
            handle_raw_read(freq->lba, freq->count, resp);
            break;
        default:
            resp->status = -EINVAL;
            break;
        }

        // Send reply: header + data bytes (count)
        size_t resp_len = sizeof(struct file_resp) + resp->count;
        msg_resp(resp, resp_len);
    }
}
