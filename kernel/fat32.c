/* kernel/fat32.c — In-kernel FAT32 filesystem (synchronous)
 * Ported from driver/fs_driver.cc, converting async state machines
 * to simple synchronous loops using blk_read/blk_write.
 *
 * Kernel constraint: C only (commit 18c91ca).
 */
#include "kernel/fat32.h"
#include "kernel/blk_dev.h"
#include "kernel/inode.h"
#include "kernel/page_cache.h"
#include "common/stat.h"
#include "common/dirent.h"
#include "kernel/spinlock.h"
#include "kernel/serial.h"
#include "kernel/mem/slab.h"
#include "common/errno.h"
#include "common/fcntl.h"
#include "arch/x64/utils.h"
#include <stddef.h>

/* Forward declarations for static functions used before definition */
static int fat32_create_file(const char *path);

/* ==================== FAT32 volume state ==================== */
static uint32_t part_start_lba;
static uint32_t fat_start_lba;
static uint32_t data_start_lba;
static uint32_t root_cluster;
static uint32_t sectors_per_cluster;
static uint32_t bytes_per_cluster;
static uint32_t total_data_clusters;
static uint32_t spf32;
static uint32_t next_free_hint = 2;

/* Global FAT lock: protects FAT modifications (free cluster scan + FAT entry writes).
 * Lock order: i_lock -> fat_lock -> ahci_lock (via blk_write) */
static spinlock_t fat_lock = SPINLOCK_INIT;

/* ==================== FAT sector cache (16 slots, LRU) ==================== */
#define FAT_CACHE_PAGES 16

struct fat_cache_entry {
    uint32_t sector_lba;
    uint8_t  data[512];
};

static struct fat_cache_entry fat_cache[FAT_CACHE_PAGES];
static uint32_t fat_cache_time = 0;
static uint32_t fat_cache_age[FAT_CACHE_PAGES];
static spinlock_t fat_cache_lock = SPINLOCK_INIT;

static int fat_cache_lookup(uint32_t sector_lba) {
    spin_lock(&fat_cache_lock);
    for (int i = 0; i < FAT_CACHE_PAGES; i++) {
        if (fat_cache[i].sector_lba == sector_lba) {
            fat_cache_age[i] = ++fat_cache_time;
            spin_unlock(&fat_cache_lock);
            return i;
        }
    }
    spin_unlock(&fat_cache_lock);
    return -1;
}

static int fat_cache_alloc(uint32_t sector_lba) {
    spin_lock(&fat_cache_lock);
    int best = 0;
    for (int i = 1; i < FAT_CACHE_PAGES; i++) {
        if (fat_cache_age[i] < fat_cache_age[best]) best = i;
    }
    fat_cache[best].sector_lba = sector_lba;
    fat_cache_age[best] = ++fat_cache_time;
    spin_unlock(&fat_cache_lock);
    return best;
}

/* Read FAT sector into cache, returns cache slot */
static int fat_cache_read(uint32_t sector_lba) {
    int slot = fat_cache_lookup(sector_lba);
    if (slot >= 0) return slot;
    slot = fat_cache_alloc(sector_lba);
    if (blk_read_sector(sector_lba, fat_cache[slot].data) != 0) return -1;
    return slot;
}

/* Invalidate FAT cache entries for a given sector */
static void fat_cache_invalidate_sector(uint32_t sector_lba) {
    spin_lock(&fat_cache_lock);
    for (int i = 0; i < FAT_CACHE_PAGES; i++) {
        if (fat_cache[i].sector_lba == sector_lba) {
            fat_cache[i].sector_lba = 0xFFFFFFFF;
            fat_cache_age[i] = 0;
        }
    }
    spin_unlock(&fat_cache_lock);
}

/* ==================== FAT entry read/write ==================== */

/* Read a FAT entry (synchronous, uses FAT cache) */
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

/* Write a FAT entry (dual-write to FAT1 and FAT2) */
static int fat32_write_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;

    uint8_t sector_buf[512];
    if (blk_read_sector(fat_sector, sector_buf) != 0) return -EIO;

    uint8_t *p = sector_buf + offset_in_sector;
    uint32_t old = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t nv = (old & 0xF0000000) | (value & 0x0FFFFFFF);
    p[0] = nv & 0xFF;
    p[1] = (nv >> 8) & 0xFF;
    p[2] = (nv >> 16) & 0xFF;
    p[3] = (nv >> 24) & 0xFF;

    /* Write FAT1 */
    if (blk_write(fat_sector, 1, sector_buf) != 0) return -EIO;
    /* Write FAT2 */
    if (blk_write(fat_sector + spf32, 1, sector_buf) != 0) return -EIO;

    /* Invalidate cache for this sector so subsequent reads get fresh data */
    fat_cache_invalidate_sector(fat_sector);
    fat_cache_invalidate_sector(fat_sector + spf32);

    return 0;
}

/* ==================== FAT chain walk ==================== */

/* Walk FAT chain from start_cluster, return the cluster at page_index */
uint32_t fat32_walk_chain(uint32_t start_cluster, uint64_t page_index) {
    uint32_t c = start_cluster;
    for (uint64_t i = 0; i < page_index; i++) {
        if (c < 2 || c >= 0x0FFFFFF8) return c;
        uint32_t next = fat_read_entry(c);
        if (next >= 0x0FFFFFF8) return next;
        c = next;
    }
    return c;
}

/* ==================== Cluster allocation ==================== */

/* Find a free cluster and mark it as EOF. Returns cluster number or 0 on failure. */
static uint32_t fat32_allocate_cluster(void) {
    for (uint32_t sector = 0; sector < spf32; sector++) {
        uint32_t abs_sector = ((next_free_hint / 128) + sector) % spf32;
        int slot = fat_cache_read(fat_start_lba + abs_sector);
        if (slot < 0) continue;

        uint8_t *fd = fat_cache[slot].data;
        for (int i = 0; i < 128; i++) {
            uint32_t c = abs_sector * 128 + i;
            if (c < 2 || c >= total_data_clusters + 2) continue;
            uint32_t e = (uint32_t)fd[i*4] | ((uint32_t)fd[i*4+1] << 8) |
                          ((uint32_t)fd[i*4+2] << 16) | ((uint32_t)fd[i*4+3] << 24);
            e &= 0x0FFFFFFF;
            if (e == 0) {
                /* Found free cluster — mark as EOF in FAT */
                next_free_hint = c + 1;
                if (next_free_hint >= total_data_clusters + 2) next_free_hint = 2;
                if (fat32_write_fat_entry(c, 0x0FFFFFFF) != 0) return 0;
                return c;
            }
        }
    }
    return 0; /* ENOSPC */
}

/* Free an entire cluster chain starting from start_cluster */
static void fat32_free_chain(uint32_t start_cluster) {
    uint32_t c = start_cluster;
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t next = fat_read_entry(c);
        fat32_write_fat_entry(c, 0);
        if (c < next_free_hint) next_free_hint = c;
        if (next >= 0x0FFFFFF8) break;
        c = next;
    }
}

/* Link a new cluster after tail_cluster in the FAT chain */
static int fat32_link_cluster(uint32_t tail_cluster, uint32_t new_cluster) {
    return fat32_write_fat_entry(tail_cluster, new_cluster);
}

/* ==================== 8.3 name helpers ==================== */

static void format_83_name(const char *user, int user_len, uint8_t out[11]) {
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

static int match_83_name(const uint8_t stored[11], const char *name, int name_len) {
    uint8_t expanded[11];
    format_83_name(name, name_len, expanded);
    for (int i = 0; i < 11; i++) {
        if (stored[i] != expanded[i]) return 0;
    }
    return 1;
}

/* ==================== LFN helpers ==================== */

static int collect_lfn_entry(const struct fat_dir_entry *de, char *lfn_buf) {
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
            return 0;
        }
        if (lo == 0x00 || lo == 0xFF) {
            lfn_buf[base + c] = '\0';
            for (int k = base + c + 1; k < 256; k++) lfn_buf[k] = '\0';
            return 1;
        }
        lfn_buf[base + c] = (char)lo;
    }
    int is_last = raw[0] & 0x40;
    if (is_last)
        lfn_buf[base + n_chars] = '\0';
    return 1;
}

static int match_lfn_name(const char *lfn_buf, const char *name, int name_len) {
    for (int i = 0; i < name_len; i++) {
        char lc = lfn_buf[i];
        char nc = name[i];
        if (lc == '\0') return 0;
        if (lc >= 'a' && lc <= 'z') lc -= 32;
        if (nc >= 'a' && nc <= 'z') nc -= 32;
        if (lc != nc) return 0;
    }
    return lfn_buf[name_len] == '\0';
}

/* ==================== Volume geometry accessors ==================== */

uint32_t fat32_data_start_lba(void) { return data_start_lba; }
uint32_t fat32_sectors_per_cluster(void) { return sectors_per_cluster; }
uint32_t fat32_bytes_per_cluster(void) { return bytes_per_cluster; }

/* ==================== Path resolution (synchronous) ==================== */

static int fat32_resolve_path(const char *path, uint32_t *out_cluster,
                       uint32_t *out_dir_cluster, int *out_dir_entry_idx,
                       uint64_t *out_file_size, int is_parent) {
    if (!path || path[0] != '/') return -ENOENT;

    /* Root directory */
    if (path[1] == '\0') {
        *out_cluster = root_cluster;
        *out_dir_cluster = root_cluster;
        *out_dir_entry_idx = -1;
        *out_file_size = 0;
        return 0;
    }

    /* For is_parent mode: find last slash,
     * resolve the parent path, then return parent dir info */
    int leaf_len = 0;
    int parent_end = -1; /* index of last '/' in path */

    if (is_parent) {
        int path_len = 0;
        while (path[path_len]) path_len++;
        for (int i = path_len - 1; i >= 0; i--) {
            if (path[i] == '/') { parent_end = i; break; }
        }
        if (parent_end < 0) return -ENOENT;
        if (parent_end == 0) {
            /* Parent is root */
            *out_cluster = root_cluster;
            *out_dir_cluster = root_cluster;
            *out_dir_entry_idx = -1;
            *out_file_size = 0;
            return 0;
        }
        /* Extract leaf name length after last slash (not stored, just advance past it) */
        const char *ls = path + parent_end + 1;
        while (ls[leaf_len] && leaf_len < 255) {
            leaf_len++;
        }
    }

    uint32_t cluster = root_cluster;
    uint32_t prev_dir_cluster = root_cluster;
    const char *p = path + 1; /* skip leading '/' */

    while (*p) {
        /* Extract next path component */
        char component[256];
        int clen = 0;
        while (*p && *p != '/' && clen < 255) component[clen++] = *p++;
        component[clen] = '\0';
        if (*p == '/') p++;

        int is_last = (*p == '\0');

        /* In is_parent mode: if the remaining path is past parent_end,
         * treat this component as the last one (it's the parent directory) */
        int is_parent_last = 0;
        if (is_parent && (p - path) > parent_end) {
            is_parent_last = 1;
        }

        /* Handle "." and ".." */
        if (component[0] == '.' && component[1] == '\0') {
            /* "." — stay in current directory */
            if (is_last || is_parent_last) {
                *out_cluster = cluster;
                *out_dir_cluster = prev_dir_cluster;
                *out_dir_entry_idx = -1;
                *out_file_size = 0;
                return 0;
            }
            continue;
        }
        if (component[0] == '.' && component[1] == '.' && component[2] == '\0') {
            /* ".." — go to parent directory.
             * Read the ".." entry to find parent cluster.
             * For root, ".." points to root itself. */
            if (cluster != root_cluster) {
                /* Scan current dir for ".." entry to get parent cluster */
                uint32_t parent_cluster = root_cluster; /* default */
                uint32_t scan2 = cluster;
                while (scan2 >= 2 && scan2 < 0x0FFFFFF8) {
                    uint32_t lba2 = data_start_lba + (scan2 - 2) * sectors_per_cluster;
                    uint8_t *buf2 = (uint8_t *)kmalloc(bytes_per_cluster);
                    if (!buf2) return -ENOMEM;
                    if (blk_read(lba2, sectors_per_cluster, buf2) != 0) {
                        kfree(buf2);
                        return -EIO;
                    }
                    int entries2 = bytes_per_cluster / 32;
                    for (int j = 0; j < entries2; j++) {
                        struct fat_dir_entry *de2 = (struct fat_dir_entry *)(buf2 + j * 32);
                        if (de2->name[0] == 0x00) break;
                        if (de2->name[0] == 0xE5) continue;
                        if (de2->attr == 0x0F) continue;
                        if (de2->name[0] == '.' && de2->name[1] == '.') {
                            uint32_t pc = ((uint32_t)de2->fst_clus_hi << 16) | de2->fst_clus_lo;
                            if (pc == 0) pc = root_cluster;
                            parent_cluster = pc;
                            kfree(buf2);
                            goto dotdot_done;
                        }
                    }
                    kfree(buf2);
                    scan2 = fat_read_entry(scan2);
                }
dotdot_done:
                prev_dir_cluster = cluster;
                cluster = parent_cluster;
            }
            /* else: already at root, stay */
            if (is_last || is_parent_last) {
                *out_cluster = cluster;
                *out_dir_cluster = prev_dir_cluster;
                *out_dir_entry_idx = -1;
                *out_file_size = 0;
                return 0;
            }
            continue;
        }

        /* Scan current directory for this component */
        int found = 0;
        uint32_t scan_cluster = cluster;
        char lfn_buf[256];
        __memset(lfn_buf, 0, sizeof(lfn_buf));

        while (scan_cluster >= 2 && scan_cluster < 0x0FFFFFF8) {
            uint32_t lba = data_start_lba + (scan_cluster - 2) * sectors_per_cluster;
            uint8_t *buf = (uint8_t *)kmalloc(bytes_per_cluster);
            if (!buf) return -ENOMEM;
            if (blk_read(lba, sectors_per_cluster, buf) != 0) {
                kfree(buf);
                return -EIO;
            }

            int entries = bytes_per_cluster / 32;
            for (int i = 0; i < entries; i++) {
                struct fat_dir_entry *de = (struct fat_dir_entry *)(buf + i * 32);
                if (de->name[0] == 0x00) {
                    /* End of directory */
                    kfree(buf);
                    return -ENOENT;
                }
                if (de->name[0] == 0xE5) {
                    lfn_buf[0] = '\0';
                    continue;
                }
                if (de->attr == 0x0F) {
                    collect_lfn_entry(de, lfn_buf);
                    continue;
                }

                /* Short name entry — check match */
                int matched = 0;
                if (lfn_buf[0] != '\0')
                    matched = match_lfn_name(lfn_buf, component, clen);
                if (!matched)
                    matched = match_83_name(de->name, component, clen);
                lfn_buf[0] = '\0';

                if (matched) {
                    uint32_t entry_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
                    if (entry_cluster == 0 && (de->attr & 0x10)) entry_cluster = root_cluster;

                    if (is_last || is_parent_last) {
                        /* Found the target */
                        *out_cluster = entry_cluster;
                        *out_dir_cluster = scan_cluster;
                        *out_dir_entry_idx = i;
                        *out_file_size = de->file_size;
                        kfree(buf);
                        return 0;
                    }

                    /* Intermediate component — descend */
                    if (!(de->attr & 0x10)) {
                        kfree(buf);
                        return -ENOTDIR;
                    }
                    prev_dir_cluster = scan_cluster;
                    cluster = entry_cluster;
                    found = 1;
                    break;
                }
            }

            kfree(buf);
            if (found) break;

            /* Follow FAT chain */
            uint32_t next = fat_read_entry(scan_cluster);
            if (next >= 0x0FFFFFF8) return -ENOENT;
            scan_cluster = next;
        }

        if (!found) return -ENOENT;
    }

    return -ENOENT;
}

/* ==================== Directory entry read/write helpers ==================== */

/* Read a cluster from disk into a kmalloc'd buffer */
static uint8_t *read_cluster_buf(uint32_t cluster) {
    uint8_t *buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!buf) return NULL;
    uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;
    if (blk_read(lba, sectors_per_cluster, buf) != 0) {
        kfree(buf);
        return NULL;
    }
    return buf;
}

/* Write back a single sector from a cluster buffer */
static int write_cluster_sector(uint32_t cluster, int sector_idx, const uint8_t *data) {
    uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster + sector_idx;
    return blk_write(lba, 1, data);
}

/* Update directory entry on disk */
static int fat32_update_dir_entry(uint32_t dir_cluster, int dir_idx,
                           uint32_t start_cluster, uint32_t file_size) {
    uint8_t *buf = read_cluster_buf(dir_cluster);
    if (!buf) return -EIO;

    struct fat_dir_entry *de = (struct fat_dir_entry *)(buf + dir_idx * 32);
    de->fst_clus_hi = (start_cluster >> 16) & 0xFFFF;
    de->fst_clus_lo = start_cluster & 0xFFFF;
    de->file_size = file_size;

    int sector_idx = (dir_idx * 32) / 512;
    int rc = write_cluster_sector(dir_cluster, sector_idx, buf + sector_idx * 512);
    kfree(buf);
    return rc;
}

/* ==================== Truncate (free cluster chain, zero size) ==================== */

static int fat32_truncate(uint32_t cluster, uint32_t dir_cluster, int dir_idx) {
    spin_lock(&fat_lock);
    fat32_free_chain(cluster);
    spin_unlock(&fat_lock);

    /* Update directory entry: cluster=0, size=0 */
    return fat32_update_dir_entry(dir_cluster, dir_idx, 0, 0);
}

/* ==================== FAT32 init ==================== */

int fat32_init(void) {
    serial_printf("fat32_init: starting\n");

    /* Initialize FAT cache */
    for (int i = 0; i < FAT_CACHE_PAGES; i++) {
        fat_cache[i].sector_lba = 0xFFFFFFFF;
        fat_cache_age[i] = 0;
    }

    /* Read MBR (LBA 0) */
    uint8_t mbr[512];
    if (blk_read_sector(0, mbr) != 0) {
        serial_printf("fat32_init: MBR read failed\n");
        return -EIO;
    }

    /* Scan partition table for FAT32 (type 0x0B or 0x0C) */
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

    /* Fallback: if no partition found, try LBA 201 (current disk layout) */
    if (part_start_lba == 0) {
        serial_printf("fat32_init: no FAT32 partition in MBR, trying LBA 201\n");
        part_start_lba = 201;
    }

    /* Read BPB */
    uint8_t bpb[512];
    if (blk_read_sector(part_start_lba, bpb) != 0) {
        serial_printf("fat32_init: BPB read failed\n");
        return -EIO;
    }

    uint16_t bps = (uint16_t)bpb[11] | ((uint16_t)bpb[12] << 8);
    sectors_per_cluster = bpb[13];
    uint16_t reserved = (uint16_t)bpb[14] | ((uint16_t)bpb[15] << 8);
    spf32 = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
            ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    root_cluster = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                   ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);

    if (bps != 512 || sectors_per_cluster == 0 || spf32 == 0 || root_cluster < 2) {
        serial_printf("fat32_init: invalid BPB (bps=%u spc=%u spf=%u root=%u)\n",
                       bps, sectors_per_cluster, spf32, root_cluster);
        return -EINVAL;
    }

    fat_start_lba = part_start_lba + reserved;
    data_start_lba = fat_start_lba + spf32 * 2;
    bytes_per_cluster = sectors_per_cluster * 512;

    if (part_total_sectors > 0) {
        uint32_t data_sectors = part_total_sectors - (data_start_lba - part_start_lba);
        total_data_clusters = data_sectors / sectors_per_cluster;
    } else {
        total_data_clusters = 0;
    }

    /* Pre-warm FAT cache */
    for (uint32_t s = 0; s < spf32; s++) {
        fat_cache_read(fat_start_lba + s);
    }

    serial_printf("fat32_init: part=%u fat=%u data=%u root=%u spc=%u bpc=%u total_cl=%u\n",
                   part_start_lba, fat_start_lba, data_start_lba,
                   root_cluster, sectors_per_cluster, bytes_per_cluster,
                   total_data_clusters);
    return 0;
}

/* ==================== File open ==================== */

struct inode *fat32_open(const char *path, int flags, int *out_errno) {
    uint32_t cluster, dir_cluster;
    int dir_idx;
    uint64_t file_size;

    int rc = fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx,
                                &file_size, 0);
    /* O_CREAT: create file if not found */
    if (rc != 0 && (flags & O_CREAT)) {
        spin_lock(&fat_lock);
        rc = fat32_create_file(path);
        spin_unlock(&fat_lock);
        if (rc != 0) { *out_errno = rc; return NULL; }
        rc = fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx,
                                &file_size, 0);
        if (rc != 0) { *out_errno = rc; return NULL; }
    }
    if (rc != 0) { *out_errno = rc; return NULL; }

    /* O_TRUNC: truncate file */
    if ((flags & O_TRUNC) && file_size > 0) {
        fat32_truncate(cluster, dir_cluster, dir_idx);
        file_size = 0;
    }

    /* Determine if this is a directory */
    int is_dir = 0;
    if (dir_idx < 0) {
        /* Root directory */
        is_dir = 1;
    } else {
        uint8_t *dir_buf = read_cluster_buf(dir_cluster);
        if (dir_buf) {
            struct fat_dir_entry *de = (struct fat_dir_entry *)(dir_buf + dir_idx * 32);
            if (de->attr & 0x10) is_dir = 1;
            kfree(dir_buf);
        }
    }

    /* Look up or create inode */
    int ino_type = is_dir ? INODE_DIR : INODE_REGULAR;
    struct inode *ip = inode_lookup(cluster);
    if (!ip) {
        ip = inode_create(cluster, ino_type, file_size,
                          cluster, dir_cluster, dir_idx);
    }
    return ip;
}

/* ==================== File read ==================== */

int fat32_read(struct inode *ip, uint64_t offset, void *buf, size_t count) {
    if (offset >= ip->size) return 0;
    uint64_t avail = ip->size - offset;
    if (count > avail) count = (size_t)avail;

    size_t nread = 0;
    while (nread < count) {
        uint64_t page_idx = (offset + nread) / 4096;
        uint32_t page_off = (offset + nread) % 4096;
        uint32_t chunk = 4096 - page_off;
        if (chunk > count - nread) chunk = count - nread;

        struct cache_page *cp = page_cache_fill(ip, page_idx);
        if (!cp) break;
        __memcpy((uint8_t *)buf + nread, cp->data + page_off, chunk);
        page_cache_release(cp);
        nread += chunk;
    }
    return (int)nread;
}

/* ==================== File write ==================== */

int fat32_write(struct inode *ip, uint64_t offset, const void *buf, size_t count) {
    spin_lock(&ip->i_lock);

    /* O_APPEND: write at end of file */
    if (ip->mode & O_APPEND) {
        offset = ip->size;
    }

    size_t written = 0;
    while (written < count) {
        uint64_t page_idx = (offset + written) / 4096;
        uint32_t page_off = (offset + written) % 4096;
        uint32_t chunk = 4096 - page_off;
        if (chunk > count - written) chunk = count - written;

        /* Convert page_idx to cluster index for FAT chain walk */
        uint32_t clusters_per_page = 4096 / bytes_per_cluster;
        uint32_t cluster_idx = page_idx * clusters_per_page;
        /* For write, we need the cluster corresponding to the write offset within the page */
        uint32_t cluster_in_page = page_off / bytes_per_cluster;
        uint32_t target_cluster = fat32_walk_chain(ip->start_cluster, cluster_idx + cluster_in_page);
        if (target_cluster < 2 || target_cluster >= 0x0FFFFFF8) {
            /* Need to allocate a new cluster */
            spin_lock(&fat_lock);
            uint32_t new_cluster = fat32_allocate_cluster();
            if (new_cluster == 0) {
                spin_unlock(&fat_lock);
                break; /* ENOSPC */
            }
            /* Zero-fill new cluster */
            uint8_t zero_buf[4096];
            __memset(zero_buf, 0, bytes_per_cluster);
            uint32_t lba = data_start_lba + (new_cluster - 2) * sectors_per_cluster;
            blk_write(lba, sectors_per_cluster, zero_buf);

            /* Link into chain */
            if (ip->start_cluster == 0 || ip->size == 0) {
                /* First cluster for this file */
                ip->start_cluster = new_cluster;
                ip->dir_start_cluster = ip->dir_start_cluster;
                ip->dir_entry_index = ip->dir_entry_index;
            } else {
                /* Find tail of chain and link */
                uint32_t tail = ip->start_cluster;
                while (1) {
                    uint32_t next = fat_read_entry(tail);
                    if (next >= 0x0FFFFFF8) break;
                    tail = next;
                }
                fat32_link_cluster(tail, new_cluster);
            }
            spin_unlock(&fat_lock);

            /* Invalidate page cache — new cluster changes mapping */
            page_cache_invalidate_inode(ip);
            target_cluster = new_cluster;
        }

        struct cache_page *cp = page_cache_fill(ip, page_idx);
        if (!cp) break;
        __memcpy(cp->data + page_off, (const uint8_t *)buf + written, chunk);
        page_cache_mark_dirty(cp);
        page_cache_writeback(cp);
        page_cache_release(cp);
        written += chunk;
    }

    uint64_t new_end = offset + written;
    if (new_end > ip->size) {
        ip->size = new_end;
        /* Update directory entry */
        if (ip->dir_start_cluster >= 2) {
            fat32_update_dir_entry(ip->dir_start_cluster, ip->dir_entry_index,
                                   ip->start_cluster, (uint32_t)ip->size);
        }
    }

    spin_unlock(&ip->i_lock);
    return (int)written;
}

/* ==================== Create file ==================== */

static int fat32_create_file(const char *path) {
    /* Resolve parent directory */
    uint32_t parent_cluster, dummy_cluster;
    int dummy_idx;
    uint64_t dummy_size;
    int rc = fat32_resolve_path(path, &dummy_cluster, &parent_cluster,
                                &dummy_idx, &dummy_size, 1);
    if (rc != 0) {
        return rc;
    }

    /* Extract leaf name */
    int path_len = 0;
    while (path[path_len]) path_len++;
    int last_slash = -1;
    for (int i = path_len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }
    const char *leaf = path + last_slash + 1;
    int leaf_len = path_len - last_slash - 1;

    if (leaf_len == 0) return -ENOENT;

    /* Find a free directory entry slot in parent */
    uint8_t *dir_buf = read_cluster_buf(dummy_cluster);
    if (!dir_buf) return -EIO;

    int entries = bytes_per_cluster / 32;
    int free_idx = -1;
    int was_end_of_dir = 0;
    for (int i = 0; i < entries; i++) {
        struct fat_dir_entry *de = (struct fat_dir_entry *)(dir_buf + i * 32);
        if (de->name[0] == 0x00) {
            free_idx = i;
            was_end_of_dir = 1;
            break;
        }
        if (de->name[0] == 0xE5) {
            free_idx = i;
            break;
        }
    }

    if (free_idx < 0) {
        kfree(dir_buf);
        return -ENOSPC; /* TODO: extend directory chain */
    }

    /* Create the directory entry */
    struct fat_dir_entry new_entry;
    __memset(&new_entry, 0, sizeof(new_entry));
    format_83_name(leaf, leaf_len, new_entry.name);
    new_entry.attr = 0; /* regular file */
    new_entry.fst_clus_hi = 0;
    new_entry.fst_clus_lo = 0;
    new_entry.file_size = 0;

    /* Write entry to directory */
    __memcpy(dir_buf + free_idx * 32, &new_entry, 32);

    /* If we replaced the end-of-directory marker, write a new one after */
    if (was_end_of_dir && free_idx + 1 < entries) {
        __memset(dir_buf + (free_idx + 1) * 32, 0, 32);
    }

    /* Write back the affected sector(s) */
    int sector_idx = (free_idx * 32) / 512;
    int wrc = write_cluster_sector(dummy_cluster, sector_idx, dir_buf + sector_idx * 512);
    if (wrc != 0) { kfree(dir_buf); return -EIO; }
    /* Also write next sector if end-of-dir marker crosses the boundary */
    if (was_end_of_dir && free_idx + 1 < entries) {
        int next_sector = ((free_idx + 1) * 32) / 512;
        if (next_sector != sector_idx) {
            wrc = write_cluster_sector(dummy_cluster, next_sector, dir_buf + next_sector * 512);
            if (wrc != 0) { kfree(dir_buf); return -EIO; }
        }
    }
    kfree(dir_buf);
    return 0;
}

/* ==================== Mkdir ==================== */

int fat32_mkdir(const char *path) {
    /* Resolve parent directory */
    uint32_t parent_cluster, dummy_cluster;
    int dummy_idx;
    uint64_t dummy_size;
    int rc = fat32_resolve_path(path, &dummy_cluster, &parent_cluster,
                                &dummy_idx, &dummy_size, 1);
    if (rc != 0) return rc;

    /* Extract leaf name */
    int path_len = 0;
    while (path[path_len]) path_len++;
    int last_slash = -1;
    for (int i = path_len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }
    const char *leaf = path + last_slash + 1;
    int leaf_len = path_len - last_slash - 1;

    if (leaf_len == 0) return -ENOENT;

    /* Allocate a cluster for the new directory */
    uint32_t new_cluster = fat32_allocate_cluster();
    if (new_cluster == 0) return -ENOSPC;

    /* Zero-fill the cluster */
    uint8_t *dir_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!dir_buf) {
        fat32_write_fat_entry(new_cluster, 0);
        return -ENOMEM;
    }
    __memset(dir_buf, 0, bytes_per_cluster);

    /* Create "." and ".." entries */
    struct fat_dir_entry *dot = (struct fat_dir_entry *)dir_buf;
    __memset(dot, 0, 32);
    dot->name[0] = '.';
    for (int i = 1; i < 11; i++) dot->name[i] = ' ';
    dot->attr = 0x10;
    dot->fst_clus_hi = (new_cluster >> 16) & 0xFFFF;
    dot->fst_clus_lo = new_cluster & 0xFFFF;

    struct fat_dir_entry *dotdot = (struct fat_dir_entry *)(dir_buf + 32);
    __memset(dotdot, 0, 32);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    for (int i = 2; i < 11; i++) dotdot->name[i] = ' ';
    dotdot->attr = 0x10;
    dotdot->fst_clus_hi = (parent_cluster >> 16) & 0xFFFF;
    dotdot->fst_clus_lo = parent_cluster & 0xFFFF;

    /* Write new directory cluster to disk */
    uint32_t lba = data_start_lba + (new_cluster - 2) * sectors_per_cluster;
    blk_write(lba, sectors_per_cluster, dir_buf);
    kfree(dir_buf);

    /* Add entry in parent directory */
    uint8_t *parent_buf = read_cluster_buf(dummy_cluster);
    if (!parent_buf) return -EIO;

    int entries = bytes_per_cluster / 32;
    int free_idx = -1;
    for (int i = 0; i < entries; i++) {
        struct fat_dir_entry *de = (struct fat_dir_entry *)(parent_buf + i * 32);
        if (de->name[0] == 0x00 || de->name[0] == 0xE5) {
            free_idx = i;
            break;
        }
    }

    if (free_idx < 0) {
        kfree(parent_buf);
        return -ENOSPC;
    }

    struct fat_dir_entry new_entry;
    __memset(&new_entry, 0, sizeof(new_entry));
    format_83_name(leaf, leaf_len, new_entry.name);
    new_entry.attr = 0x10;
    new_entry.fst_clus_hi = (new_cluster >> 16) & 0xFFFF;
    new_entry.fst_clus_lo = new_cluster & 0xFFFF;
    new_entry.file_size = 0;

    __memcpy(parent_buf + free_idx * 32, &new_entry, 32);
    int sector_idx = (free_idx * 32) / 512;
    int wrc = write_cluster_sector(dummy_cluster, sector_idx, parent_buf + sector_idx * 512);
    kfree(parent_buf);
    return wrc == 0 ? 0 : -EIO;
}

/* ==================== Unlink ==================== */

int fat32_unlink(const char *path) {
    uint32_t cluster, dir_cluster;
    int dir_idx;
    uint64_t file_size;

    int rc = fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx, &file_size, 0);
    if (rc != 0) return rc;

    /* Read directory entry to check it's not a directory */
    uint8_t *dir_buf = read_cluster_buf(dir_cluster);
    if (!dir_buf) return -EIO;
    struct fat_dir_entry *de = (struct fat_dir_entry *)(dir_buf + dir_idx * 32);

    if (de->attr & 0x10) {
        kfree(dir_buf);
        return -EISDIR;
    }

    uint32_t target_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;

    /* Mark entry as deleted (0xE5) */
    dir_buf[dir_idx * 32] = 0xE5;
    int sector_idx = (dir_idx * 32) / 512;
    write_cluster_sector(dir_cluster, sector_idx, dir_buf + sector_idx * 512);
    kfree(dir_buf);

    /* Free cluster chain */
    if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8) {
        spin_lock(&fat_lock);
        fat32_free_chain(target_cluster);
        spin_unlock(&fat_lock);
    }

    /* Invalidate page cache for this inode */
    struct inode *ip = inode_lookup(target_cluster);
    if (ip) {
        page_cache_invalidate_inode(ip);
        inode_put(ip);
    }

    return 0;
}

/* ==================== Rmdir ==================== */

int fat32_rmdir(const char *path) {
    uint32_t cluster, dir_cluster;
    int dir_idx;
    uint64_t file_size;

    int rc = fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx, &file_size, 0);
    if (rc != 0) return rc;

    /* Check it's a directory */
    uint8_t *dir_buf = read_cluster_buf(dir_cluster);
    if (!dir_buf) return -EIO;
    struct fat_dir_entry *de = (struct fat_dir_entry *)(dir_buf + dir_idx * 32);

    if (!(de->attr & 0x10)) {
        kfree(dir_buf);
        return -ENOTDIR;
    }

    uint32_t target_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
    if (target_cluster == 0) target_cluster = root_cluster;

    /* Check directory is empty (only . and ..) */
    uint32_t cc = target_cluster;
    int is_empty = 1;
    while (cc >= 2 && cc < 0x0FFFFFF8 && is_empty) {
        uint8_t *dbuf = read_cluster_buf(cc);
        if (!dbuf) { is_empty = 0; break; }
        int entries = bytes_per_cluster / 32;
        for (int i = 0; i < entries; i++) {
            struct fat_dir_entry *d = (struct fat_dir_entry *)(dbuf + i * 32);
            if (d->name[0] == 0x00) break;
            if (d->name[0] == 0xE5) continue;
            if (d->name[0] == '.' && (d->name[1] == ' ' || d->name[1] == '.')) continue;
            if (d->name[0] == '.' && d->name[1] == '.') continue;
            is_empty = 0;
            break;
        }
        kfree(dbuf);
        uint32_t next = fat_read_entry(cc);
        if (next >= 0x0FFFFFF8) break;
        cc = next;
    }

    if (!is_empty) {
        kfree(dir_buf);
        return -EBUSY;
    }

    /* Mark entry deleted in parent */
    dir_buf[dir_idx * 32] = 0xE5;
    int sector_idx = (dir_idx * 32) / 512;
    write_cluster_sector(dir_cluster, sector_idx, dir_buf + sector_idx * 512);
    kfree(dir_buf);

    /* Free directory cluster chain */
    if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8) {
        spin_lock(&fat_lock);
        fat32_free_chain(target_cluster);
        spin_unlock(&fat_lock);
    }

    return 0;
}

/* ==================== Stat ==================== */

int fat32_stat(const char *path, void *stat_buf) {
    uint32_t cluster, dir_cluster;
    int dir_idx;
    uint64_t file_size;

    int rc = fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx, &file_size, 0);
    if (rc != 0) return rc;

    struct kstat *st = (void *)stat_buf;
    __memset(st, 0, sizeof(*st));
    st->st_ino = cluster;

    if (dir_idx < 0) {
        /* Reached via root, "." or ".." — no parent dir entry; treat as directory */
        st->st_mode = 0040755;
        st->st_nlink = 1;
        st->st_size = 0;
        st->st_blksize = bytes_per_cluster;
        return 0;
    }

    /* Read directory entry for attributes */
    uint8_t *dir_buf = read_cluster_buf(dir_cluster);
    if (!dir_buf) return -EIO;
    struct fat_dir_entry *de = (struct fat_dir_entry *)(dir_buf + dir_idx * 32);

    st->st_mode = (de->attr & 0x10) ? 0040755 : 0100644;
    st->st_nlink = 1;
    st->st_size = file_size;
    st->st_blksize = bytes_per_cluster;

    kfree(dir_buf);
    return 0;
}

/* ==================== getdents: read directory entries into user buffer ==================== */
/* Each entry: struct dirent64 — defined in common/dirent.h
 * pos tracks how many dir entries we've consumed so far.
 * Returns total bytes written, or negative errno. */

int fat32_getdents(uint32_t dir_cluster, uint64_t *pos, void *buf, size_t len) {
    uint8_t *out = (uint8_t *)buf;
    size_t written = 0;
    uint64_t entry_idx = 0;
    uint32_t scan_cluster = dir_cluster;
    char lfn_buf[256];
    __memset(lfn_buf, 0, sizeof(lfn_buf));

    while (scan_cluster >= 2 && scan_cluster < 0x0FFFFFF8) {
        uint8_t *cbuf = read_cluster_buf(scan_cluster);
        if (!cbuf) return -EIO;

        int entries = bytes_per_cluster / 32;
        for (int i = 0; i < entries; i++) {
            struct fat_dir_entry *de = (struct fat_dir_entry *)(cbuf + i * 32);
            if (de->name[0] == 0x00) {
                /* End of directory */
                kfree(cbuf);
                *pos = (uint64_t)-1; /* signal EOF */
                return (int)written;
            }
            if (de->name[0] == 0xE5) {
                lfn_buf[0] = '\0';
                continue;
            }
            if (de->attr == 0x0F) {
                collect_lfn_entry(de, lfn_buf);
                continue;
            }

            /* Skip entries we've already consumed (for resuming after pos) */
            if (entry_idx < *pos) {
                entry_idx++;
                lfn_buf[0] = '\0';
                continue;
            }

            /* Build name */
            char name[256];
            if (lfn_buf[0] != '\0') {
                int j;
                for (j = 0; lfn_buf[j] && j < 255; j++) name[j] = lfn_buf[j];
                name[j] = '\0';
            } else {
                /* Convert 8.3 name */
                int j = 0;
                for (int k = 0; k < 8 && de->name[k] != ' '; k++) {
                    char c = de->name[k];
                    if (c >= 'A' && c <= 'Z') c += 32;
                    name[j++] = c;
                }
                if (de->name[8] != ' ') {
                    name[j++] = '.';
                    for (int k = 8; k < 11 && de->name[k] != ' '; k++) {
                        char c = de->name[k];
                        if (c >= 'A' && c <= 'Z') c += 32;
                        name[j++] = c;
                    }
                }
                name[j] = '\0';
            }
            lfn_buf[0] = '\0';

            /* Compute entry size: dirent64 header + name + null + padding to 8-byte align */
            int name_len = 0;
            while (name[name_len]) name_len++;
            uint16_t reclen = (uint16_t)(sizeof(struct dirent64) + name_len + 1);
            reclen = (reclen + 7) & ~7; /* 8-byte align */

            /* If this entry doesn't fit, stop here */
            if (written + reclen > len) {
                kfree(cbuf);
                *pos = entry_idx;
                return (int)written;
            }

            /* Fill entry */
            struct dirent64 *d = (struct dirent64 *)(out + written);
            uint32_t entry_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
            if (entry_cluster == 0 && (de->attr & 0x10)) entry_cluster = root_cluster;
            d->d_ino = entry_cluster;
            d->d_reclen = reclen;
            d->d_type = (de->attr & 0x10) ? 4 : 8; /* DT_DIR=4, DT_REG=8 */
            int j;
            for (j = 0; j < name_len; j++) d->d_name[j] = name[j];
            d->d_name[j] = '\0';

            written += reclen;
            entry_idx++;
        }

        kfree(cbuf);

        /* Follow FAT chain */
        uint32_t next = fat_read_entry(scan_cluster);
        if (next >= 0x0FFFFFF8) {
            *pos = (uint64_t)-1; /* EOF */
            return (int)written;
        }
        scan_cluster = next;
    }

    *pos = (uint64_t)-1;
    return (int)written;
}
