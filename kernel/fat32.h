#ifndef KERNEL_FAT32_H
#define KERNEL_FAT32_H

#include <stdint.h>
#include <stddef.h>

/* FAT32 directory entry (32 bytes) */
struct fat_dir_entry {
    uint8_t  name[11];
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

/* Volume geometry accessors (used by page_cache) */
uint32_t fat32_data_start_lba(void);
uint32_t fat32_sectors_per_cluster(void);

/* Core operations */
int     fat32_init(void);
uint32_t fat32_walk_chain(uint32_t start_cluster, uint64_t page_index);
uint32_t fat_read_entry(uint32_t cluster);

/* Path resolution */
int fat32_resolve_path(const char *path, uint32_t *out_cluster,
                       uint32_t *out_dir_cluster, int *out_dir_entry_idx,
                       uint64_t *out_file_size, int is_parent);

/* File operations */
struct inode;
struct inode *fat32_open(const char *path, int flags, int *out_errno);
int  fat32_read(struct inode *ip, uint64_t offset, void *buf, size_t count);
int  fat32_write(struct inode *ip, uint64_t offset, const void *buf, size_t count);
int  fat32_create_file(const char *path);
int  fat32_mkdir(const char *path);
int  fat32_unlink(const char *path);
int  fat32_rmdir(const char *path);
int  fat32_stat(const char *path, void *stat_buf);
int  fat32_truncate(uint32_t cluster, uint32_t dir_cluster, int dir_idx);

/* FAT allocation */
uint32_t fat32_allocate_cluster(void);
void     fat32_free_chain(uint32_t start_cluster);
int      fat32_link_cluster(uint32_t tail_cluster, uint32_t new_cluster);
int      fat32_write_fat_entry(uint32_t cluster, uint32_t value);

/* Directory entry update */
int fat32_update_dir_entry(uint32_t dir_cluster, int dir_idx, uint32_t start_cluster, uint32_t file_size);

#endif
