// ===================== FAT32 filesystem driver (user-space) — read + write
// Reads requests from FS_REQ shared page, performs FAT32 operations,
// writes results in FS_RESP. Accesses disk via disk_driver through
// DISK_REQ/DISK_RESP shared pages.
#include <stdint.h>
#include "arch/x64/utils.h"
#include "common/shm.h"
#include "common/pid.h"

// Static buffers to avoid stack overflow (4KB arrays on stack exceed user stack page)
static uint8_t dir_cluster_data_buf[4096];   // used in handle_mkdir
static uint8_t zero_cluster_buf[4096];       // used in dir_chain_extend
static uint8_t sector_buf[512];              // used in fat_write_entry

// ===================== Shared page pointers =====================
static volatile disk_req_shm  *dreq  = (volatile disk_req_shm  *)DISK_REQ_ADDR;
static volatile disk_resp_shm *dresp = (volatile disk_resp_shm *)DISK_RESP_ADDR;
static volatile fs_req_shm    *freq  = (volatile fs_req_shm    *)FS_REQ_ADDR;
static volatile fs_resp_shm   *fresp = (volatile fs_resp_shm   *)FS_RESP_ADDR;

// ===================== FAT32 volume state =====================
static uint32_t part_start_lba;     // FAT32 partition start LBA
static uint32_t fat_start_lba;      // First FAT sector (absolute)
static uint32_t data_start_lba;     // First data sector (absolute)
static uint32_t root_cluster;       // Root directory cluster
static uint32_t sectors_per_cluster;
static uint32_t bytes_per_cluster;  // = sectors_per_cluster * 512
static uint32_t total_data_clusters; // total data clusters in volume

// ===================== Disk I/O helpers =====================
static int disk_read(uint32_t lba, uint32_t count) {
    dreq->cmd   = 0;  // READ
    dreq->lba   = lba;
    dreq->count = count;
    sys_notify(DISK_DRIVER_PID);
    sys_wait();
    return dresp->status;
}

static int disk_write(uint32_t lba, uint32_t count, const uint8_t *data) {
    dreq->cmd   = 1;  // WRITE
    dreq->lba   = lba;
    dreq->count = count;
    // Copy data into disk_req (full sector(s))
    uint32_t bytes = count * 512;
    __memcpy((void *)dreq->data, data, bytes);
    sys_notify(DISK_DRIVER_PID);
    sys_wait();
    return dresp->status;
}

// ===================== LRU cache (2 slots) =====================
#define CACHE_SLOTS 2

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

// Read a cluster, using cache if possible. Returns cache slot index or -1 on error.
static int read_cluster(uint32_t cluster) {
    int slot = cache_lookup(cluster);
    if (slot >= 0) return slot;

    slot = cache_alloc(cluster);
    uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;

    if (disk_read(lba, sectors_per_cluster) != 0) {
        cache[slot].cluster = 0xFFFFFFFF;  // invalidate
        return -1;
    }

    __memcpy(cache[slot].data, (const void *)dresp->data, bytes_per_cluster);
    return slot;
}

// ===================== FAT chain traversal =====================
static uint32_t fat_read_entry(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;

    if (disk_read(fat_sector, 1) != 0)
        return 0x0FFFFFFF;  // treat as end-of-chain on error

    uint32_t entry_val;
    uint8_t *src = (uint8_t *)dresp->data + offset_in_sector;
    entry_val = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
    return entry_val & 0x0FFFFFFF;
}

// Write a FAT entry for cluster, updating both FAT1 and FAT2
static int fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;

    // Read the FAT sector, modify, write back (FAT1)
    if (disk_read(fat_sector, 1) != 0) return 1;

    __memcpy(sector_buf, (const void *)dresp->data, 512);

    // Write the 4-byte FAT entry (mask to 28 bits, preserve top 4)
    uint32_t old_val;
    uint8_t *p = sector_buf + offset_in_sector;
    old_val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
              ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t new_val = (old_val & 0xF0000000) | (value & 0x0FFFFFFF);
    p[0] = new_val & 0xFF;
    p[1] = (new_val >> 8) & 0xFF;
    p[2] = (new_val >> 16) & 0xFF;
    p[3] = (new_val >> 24) & 0xFF;

    // Write FAT1
    if (disk_write(fat_sector, 1, sector_buf) != 0) return 2;

    // Write FAT2 (mirror at fat_start_lba + spf32 sectors)
    // spf32 was stored during fat32_init — recalculate from BPB
    // FAT2 offset = spf32 * 512 from fat_start_lba
    // We stored total_data_clusters but not spf32; we need the FAT size
    // FAT2 sector = fat_start_lba + spf32 + (fat_offset / 512)
    // Since we read the BPB earlier, let's compute spf32 from what we know
    // Actually, we can compute: FAT2_start = data_start_lba - spf32 * 2
    // So spf32 = (data_start_lba - fat_start_lba) / 2
    // But we can just use: fat2_sector = fat_sector + spf32
    // Let's store spf32 during init

    // For now, use the simpler approach: FAT2 sector = fat_sector + fat_size_in_sectors
    // fat_size_in_sectors = (data_start_lba - fat_start_lba) / 2
    uint32_t spf32 = (data_start_lba - fat_start_lba) / 2;
    uint32_t fat2_sector = fat_sector + spf32;

    if (disk_write(fat2_sector, 1, sector_buf) != 0) return 3;

    return 0;
}

// ===================== Cluster allocation =====================

// Find first free cluster (entry == 0) by linear scanning FAT
static uint32_t find_free_cluster() {
    // Each FAT sector holds 128 entries (512/4)
    // Total FAT sectors = (data_start_lba - fat_start_lba) / 2 (for one FAT copy)
    uint32_t spf32 = (data_start_lba - fat_start_lba) / 2;

    for (uint32_t sector = 0; sector < spf32; sector++) {
        if (disk_read(fat_start_lba + sector, 1) != 0)
            continue;

        uint8_t *fat_data = (uint8_t *)dresp->data;
        for (int i = 0; i < 128; i++) {
            uint32_t cluster = sector * 128 + i;
            if (cluster < 2) continue;  // clusters 0,1 are reserved
            if (cluster >= total_data_clusters + 2) return 0;  // out of range

            uint32_t entry = (uint32_t)fat_data[i*4] | ((uint32_t)fat_data[i*4+1] << 8) |
                             ((uint32_t)fat_data[i*4+2] << 16) | ((uint32_t)fat_data[i*4+3] << 24);
            entry &= 0x0FFFFFFF;
            if (entry == 0) return cluster;
        }
    }
    return 0;  // no free cluster found
}

// Allocate a free cluster: mark it as end-of-chain (0x0FFFFFFF)
static uint32_t allocate_cluster() {
    uint32_t cluster = find_free_cluster();
    if (cluster == 0) return 0;  // no free clusters

    if (fat_write_entry(cluster, 0x0FFFFFFF) != 0) return 0;

    return cluster;
}

// Extend a directory cluster chain by appending a new cluster
static int dir_chain_extend(uint32_t dir_cluster, uint32_t *new_cluster_out) {
    uint32_t new_cluster = allocate_cluster();
    if (new_cluster == 0) return 4;  // no free cluster

    // Zero-fill the new cluster
    for (int i = 0; i < bytes_per_cluster; i++) zero_cluster_buf[i] = 0;
    uint32_t new_lba = data_start_lba + (new_cluster - 2) * sectors_per_cluster;
    if (disk_write(new_lba, sectors_per_cluster, zero_cluster_buf) != 0) return 5;

    // Find the last cluster in the chain and update its FAT entry
    uint32_t cur = dir_cluster;
    uint32_t prev = cur;
    while (cur >= 2 && cur < 0x0FFFFFF8) {
        prev = cur;
        cur = fat_read_entry(cur);
    }
    // prev is now the last cluster (its entry >= 0x0FFFFFF8)
    if (fat_write_entry(prev, new_cluster) != 0) return 6;

    // Invalidate cache for the directory (old data is stale)
    cache_invalidate(dir_cluster);

    *new_cluster_out = new_cluster;
    return 0;
}

// ===================== FAT directory entry (32 bytes) =====================
struct fat_dir_entry {
    char     name[11];          // 8.3 format (space-padded)
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

// Convert user-visible name (e.g. "HELLO.TXT") to 8.3 stored format ("HELLO   TXT")
// Special cases: "." and ".." are FAT32 reserved entries with non-8.3 format
static void format_83_name(const char *user, char out[11]) {
    // Special handling for . and ..
    if (user[0] == '.' && user[1] == '\0') {
        out[0] = '.';
        for (int i = 1; i < 11; i++) out[i] = ' ';
        return;
    }
    if (user[0] == '.' && user[1] == '.' && user[2] == '\0') {
        out[0] = '.';
        out[1] = '.';
        for (int i = 2; i < 11; i++) out[i] = ' ';
        return;
    }

    for (int i = 0; i < 11; i++) out[i] = ' ';

    int i = 0, j = 0;
    while (user[i] && user[i] != '.' && j < 8) {
        char c = user[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = c;
        i++;
    }
    if (user[i] == '.') {
        i++;
        j = 8;
        while (user[i] && j < 11) {
            char c = user[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[j++] = c;
            i++;
        }
    }
}

static void convert_83_to_name(const char stored[11], char *out, int out_len) {
    int j = 0;
    for (int i = 0; i < 8 && j < out_len - 1; i++) {
        if (stored[i] != ' ') out[j++] = stored[i];
    }
    bool has_ext = false;
    for (int i = 8; i < 11; i++) {
        if (stored[i] != ' ') { has_ext = true; break; }
    }
    if (has_ext && j < out_len - 1) {
        out[j++] = '.';
        for (int i = 8; i < 11 && j < out_len - 1; i++) {
            if (stored[i] != ' ') out[j++] = stored[i];
        }
    }
    out[j] = '\0';
}

static bool match_83_name(const char stored[11], const char *user) {
    char expanded[11];
    format_83_name(user, expanded);
    for (int i = 0; i < 11; i++) {
        if (stored[i] != expanded[i]) return false;
    }
    return true;
}

// ===================== Path resolution =====================

// Find a directory entry by name within a directory cluster chain
static bool find_dir_entry(uint32_t dir_cluster, const char *name,
                           fat_dir_entry *out) {
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        int slot = read_cluster(cluster);
        if (slot < 0) return false;

        uint8_t *data = cache[slot].data;
        int entries = bytes_per_cluster / 32;
        for (int i = 0; i < entries; i++) {
            fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
            if (de->name[0] == 0x00) return false;  // end of dir
            if (de->name[0] == 0xE5) continue;       // deleted
            if (de->attr == 0x0F) continue;           // LFN entry

            if (match_83_name(de->name, name)) {
                *out = *de;
                return true;
            }
        }
        cluster = fat_read_entry(cluster);
    }
    return false;
}

// Resolve absolute path, returning the directory entry of the final component
static bool resolve_path(const char *path, fat_dir_entry *out,
                         uint32_t *out_cluster) {
    if (path[0] != '/') return false;

    uint32_t dir_cluster = root_cluster;

    // Root directory
    if (path[1] == '\0') {
        out->attr = 0x10;  // directory
        out->fst_clus_hi = (root_cluster >> 16) & 0xFFFF;
        out->fst_clus_lo = root_cluster & 0xFFFF;
        out->file_size = 0;
        *out_cluster = root_cluster;
        return true;
    }

    const char *p = path + 1;  // skip '/'
    char component[13];

    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 12) {
            component[i++] = *p++;
        }
        component[i] = '\0';
        if (*p == '/') p++;

        fat_dir_entry de;
        if (!find_dir_entry(dir_cluster, component, &de))
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
// For "/foo/bar.txt", returns cluster of "/" and leaf="BAR.TXT".
// For "/bar.txt", returns cluster of root and leaf="BAR.TXT".
static bool resolve_parent(const char *path, uint32_t *parent_cluster,
                           char *leaf_name) {
    if (path[0] != '/') return false;

    // Find last '/' to split parent and leaf
    int last_slash = -1;
    int path_len = 0;
    while (path[path_len]) {
        if (path[path_len] == '/') last_slash = path_len;
        path_len++;
    }

    // If path is just "/" — no leaf component
    if (path_len == 1) return false;

    // Extract leaf name (after last slash)
    const char *leaf_start = path + last_slash + 1;
    int leaf_len = 0;
    while (leaf_start[leaf_len] && leaf_len < 12) {
        leaf_name[leaf_len] = leaf_start[leaf_len];
        leaf_len++;
    }
    leaf_name[leaf_len] = '\0';

    // Resolve parent path
    if (last_slash == 0) {
        // Parent is root
        *parent_cluster = root_cluster;
        return true;
    }

    // Build parent path string (up to and including last_slash, but remove trailing slash unless root)
    char parent_path[256];
    for (int i = 0; i < last_slash; i++) parent_path[i] = path[i];
    // Remove trailing slash unless it's the root "/"
    if (last_slash > 1 && path[last_slash - 1] != '/') {
        parent_path[last_slash] = '\0';
    } else if (last_slash == 1) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        parent_path[last_slash] = '\0';
    }

    fat_dir_entry parent_de;
    uint32_t pcluster;
    if (!resolve_path(parent_path, &parent_de, &pcluster)) return false;
    if (!(parent_de.attr & 0x10)) return false;  // parent must be directory

    *parent_cluster = pcluster;
    return true;
}

// Find a free 32-byte slot in a directory cluster chain.
// Returns: {cluster_number, entry_index_within_cluster} or {-1, -1} on failure.
// If the directory is full, attempts to extend the chain.
struct dir_slot {
    uint32_t cluster;
    int      index;   // entry index within that cluster (0..entries_per_cluster-1)
};

static bool find_free_dir_slot(uint32_t dir_cluster, dir_slot *slot_out) {
    uint32_t cluster = dir_cluster;
    int entries_per_cluster = bytes_per_cluster / 32;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        int cache_slot = read_cluster(cluster);
        if (cache_slot < 0) return false;

        uint8_t *data = cache[cache_slot].data;
        for (int i = 0; i < entries_per_cluster; i++) {
            fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
            if (de->name[0] == 0x00 || de->name[0] == 0xE5) {
                slot_out->cluster = cluster;
                slot_out->index = i;
                return true;
            }
        }
        uint32_t next = fat_read_entry(cluster);
        if (next >= 0x0FFFFFF8) {
            // End of chain — try to extend
            uint32_t new_cluster;
            int rc = dir_chain_extend(dir_cluster, &new_cluster);
            if (rc != 0) return false;
            slot_out->cluster = new_cluster;
            slot_out->index = 0;
            return true;
        }
        cluster = next;
    }
    return false;
}

// Write a directory entry at a specific slot position in a cluster chain
// This writes the sector containing the entry to disk
static int write_dir_entry_at(uint32_t cluster, int index, const fat_dir_entry *entry) {
    // Read the cluster into cache
    int cache_slot = read_cluster(cluster);
    if (cache_slot < 0) return 1;

    // Modify the entry in cache
    uint8_t *data = cache[cache_slot].data;
    __memcpy(data + index * 32, entry, 32);

    // Determine which sector(s) within the cluster contain this entry
    int entry_offset = index * 32;
    int sector_in_cluster = entry_offset / 512;
    uint32_t cluster_lba = data_start_lba + (cluster - 2) * sectors_per_cluster;

    // Write the affected sector to disk
    // For simplicity, write the entire cluster (all sectors)
    // This ensures cache and disk are consistent
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (disk_write(cluster_lba + s, 1, data + s * 512) != 0) return 2;
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
    // 1. Read MBR (LBA 0)
    disk_read(0, 1);
    uint8_t *mbr = (uint8_t *)dresp->data;

    // Find FAT32 partition (type 0x0B or 0x0C) in MBR partition table
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

    // 2. Read BPB (partition start LBA)
    disk_read(part_start_lba, 1);
    uint8_t *bpb = (uint8_t *)dresp->data;

    uint16_t bps = (uint16_t)bpb[11] | ((uint16_t)bpb[12] << 8);
    sectors_per_cluster = bpb[13];
    uint16_t reserved = (uint16_t)bpb[14] | ((uint16_t)bpb[15] << 8);
    uint32_t spf32 = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
                     ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    root_cluster = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                   ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);

    // Sanity checks — if disk_driver wasn't ready, BPB may be zeros
    if (bps != 512 || sectors_per_cluster == 0 || spf32 == 0 || root_cluster < 2) {
        sys_putc('E'); sys_putc('B'); sys_putc('P'); sys_putc('B'); sys_putc('\n');
        sys_wait();  // never recover, halt
    }

    fat_start_lba = part_start_lba + reserved;
    data_start_lba = fat_start_lba + spf32 * 2;  // 2 FATs
    bytes_per_cluster = sectors_per_cluster * 512;

    // Calculate total data clusters
    uint32_t data_sectors = part_total_sectors - (data_start_lba - part_start_lba);
    total_data_clusters = data_sectors / sectors_per_cluster;
}

// ===================== Timestamp =====================
// Hardcoded timestamp: 2026-01-01 00:00:00
// FAT32 date: ((year-1980)<<9) | (month<<5) | day = (46<<9)|(1<<5)|1 = 0x5A21
// FAT32 time: (hour<<11) | (min<<5) | sec/2 = 0x0000
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

    uint32_t cluster = dir_cluster;
    uint32_t entry_count = 0;
    fs_dirent *out = (fs_dirent *)fresp->data;
    uint32_t max_entries = 8176 / sizeof(fs_dirent);

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        int slot = read_cluster(cluster);
        if (slot < 0) { fresp->status = 3; return; }

        uint8_t *data = cache[slot].data;
        int entries = bytes_per_cluster / 32;
        for (int i = 0; i < entries; i++) {
            fat_dir_entry *fde = (fat_dir_entry *)(data + i * 32);
            if (fde->name[0] == 0x00) goto done;
            if (fde->name[0] == 0xE5) continue;
            if (fde->attr == 0x0F) continue;
            if (fde->name[0] == '.') continue;

            convert_83_to_name(fde->name, out[entry_count].name, 28);
            out[entry_count].size = fde->file_size;
            out[entry_count].date = fde->wrt_date;
            out[entry_count].attr = fde->attr;
            entry_count++;
            if (entry_count >= max_entries) goto done;
        }
        cluster = fat_read_entry(cluster);
    }
done:
    fresp->status = 0;
    fresp->total = entry_count;
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

// touch: create empty file or update timestamp if exists
static void handle_create(const char *path) {
    // First check if the file already exists
    fat_dir_entry existing;
    uint32_t existing_cluster;
    if (resolve_path(path, &existing, &existing_cluster)) {
        // File/directory exists — update timestamp only (touch semantics)
        // We need to find the directory entry in its parent directory and update wrt_date/wrt_time
        uint32_t parent_cluster;
        char leaf_name[13];
        if (!resolve_parent(path, &parent_cluster, leaf_name)) {
            fresp->status = 1;
            return;
        }

        // Find the entry slot in the parent directory
        uint32_t cluster = parent_cluster;
        while (cluster >= 2 && cluster < 0x0FFFFFF8) {
            int cache_slot = read_cluster(cluster);
            if (cache_slot < 0) { fresp->status = 1; return; }

            uint8_t *data = cache[cache_slot].data;
            int entries = bytes_per_cluster / 32;
            for (int i = 0; i < entries; i++) {
                fat_dir_entry *de = (fat_dir_entry *)(data + i * 32);
                if (de->name[0] == 0x00) break;
                if (de->name[0] == 0xE5) continue;
                if (de->attr == 0x0F) continue;

                if (match_83_name(de->name, leaf_name)) {
                    // Update timestamp in cache
                    de->wrt_date = HARD_DATE;
                    de->wrt_time = HARD_TIME;
                    de->lst_acc_date = HARD_DATE;

                    // Write affected sector(s) back to disk
                    int entry_offset = i * 32;
                    int sector_in_cluster = entry_offset / 512;
                    uint32_t cluster_lba = data_start_lba + (cluster - 2) * sectors_per_cluster;
                    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
                        if (disk_write(cluster_lba + s, 1, data + s * 512) != 0) {
                            fresp->status = 1;
                            return;
                        }
                    }
                    fresp->status = 0;
                    return;
                }
            }
            cluster = fat_read_entry(cluster);
        }
        fresp->status = 1;
        return;
    }

    // File doesn't exist — create it
    uint32_t parent_cluster;
    char leaf_name[13];
    if (!resolve_parent(path, &parent_cluster, leaf_name)) {
        fresp->status = 1;
        return;
    }

    // Find a free slot in parent directory
    dir_slot slot;
    if (!find_free_dir_slot(parent_cluster, &slot)) {
        fresp->status = 4;  // no free cluster / directory full
        return;
    }

    // Create directory entry for empty file
    fat_dir_entry new_entry;
    for (int i = 0; i < 11; i++) new_entry.name[i] = ' ';
    format_83_name(leaf_name, new_entry.name);
    new_entry.attr = 0x20;          // archive (normal file)
    new_entry.nt_res = 0;
    new_entry.crt_time_tenth = 0;
    new_entry.crt_time = HARD_TIME;
    new_entry.crt_date = HARD_DATE;
    new_entry.lst_acc_date = HARD_DATE;
    new_entry.fst_clus_hi = 0;      // empty file, no cluster
    new_entry.wrt_time = HARD_TIME;
    new_entry.wrt_date = HARD_DATE;
    new_entry.fst_clus_lo = 0;
    new_entry.file_size = 0;

    int rc = write_dir_entry_at(slot.cluster, slot.index, &new_entry);
    if (rc != 0) {
        fresp->status = rc;
        return;
    }

    fresp->status = 0;
}

// mkdir: create directory with . and .. entries
static void handle_mkdir(const char *path) {
    // Check if already exists
    fat_dir_entry existing;
    uint32_t existing_cluster;
    if (resolve_path(path, &existing, &existing_cluster)) {
        fresp->status = 3;  // already exists
        return;
    }

    uint32_t parent_cluster;
    char leaf_name[13];
    if (!resolve_parent(path, &parent_cluster, leaf_name)) {
        fresp->status = 1;
        return;
    }

    // Allocate a cluster for the new directory
    uint32_t new_dir_cluster = allocate_cluster();
    if (new_dir_cluster == 0) {
        fresp->status = 4;  // no free cluster
        return;
    }

    // Initialize new directory cluster with . and .. entries + 0x00 end marker
    for (int i = 0; i < bytes_per_cluster; i++) dir_cluster_data_buf[i] = 0;

    // . entry (self-reference)
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

    // .. entry (parent reference)
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

    // Entry at index 2: 0x00 (end of directory marker, already zero from init)

    // Write the new directory cluster to disk
    uint32_t new_dir_lba = data_start_lba + (new_dir_cluster - 2) * sectors_per_cluster;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (disk_write(new_dir_lba + s, 1, dir_cluster_data_buf + s * 512) != 0) {
            fresp->status = 5;
            return;
        }
    }

    // Find a free slot in parent directory for the new directory entry
    dir_slot slot;
    if (!find_free_dir_slot(parent_cluster, &slot)) {
        fresp->status = 4;
        return;
    }

    // Create directory entry in parent
    fat_dir_entry new_entry;
    for (int i = 0; i < 11; i++) new_entry.name[i] = ' ';
    format_83_name(leaf_name, new_entry.name);
    new_entry.attr = 0x10;          // directory attribute
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

    int rc = write_dir_entry_at(slot.cluster, slot.index, &new_entry);
    if (rc != 0) {
        fresp->status = rc;
        return;
    }

    fresp->status = 0;
}

// ===================== Main loop =====================
extern "C" void _start() {
    int32_t my_pid = (int32_t)sys_getpid();
    (void)my_pid;
    int32_t shell_pid = SHELL_PID;

    // Initialize cache
    for (int i = 0; i < CACHE_SLOTS; i++) {
        cache[i].cluster = 0xFFFFFFFF;
        cache[i].age = 0;
    }

    // Initialize open file table
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        open_table[i].used = false;
    }

    // Mount FAT32
    fat32_init();

    while (1) {
        sys_wait();

        uint32_t cmd = freq->cmd;
        int32_t client_pid = (int32_t)freq->client_pid;

        // Clear response
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

        sys_notify(client_pid);
    }
}