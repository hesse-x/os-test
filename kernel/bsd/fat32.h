/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_FAT32_H
#define KERNEL_FAT32_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/bsd/fops.h"
#include "kernel/bsd/mount.h"

struct inode;
struct block_partition;

enum fat32_walk_source {
  FAT32_WALK_DEMAND = 0,
  FAT32_WALK_READAHEAD = 1,
};

struct fat32_stats {
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t cache_fill_waits;
  uint64_t cache_io_commands;
  uint64_t cache_io_sectors;
  uint64_t walk_calls[2];
  uint64_t walk_steps[2];
  uint64_t walk_head_restarts[2];
  uint64_t walk_backtracks[2];
  uint64_t walk_invalid[2];
  uint64_t mapped_sectors[2];
};

struct fat32_inode_info {
  uint32_t start_cluster;
  uint32_t dir_start_cluster;
  int32_t dir_entry_index;
  uint64_t walk_cursor;
};
extern const struct file_operations fat32_file_fops;
/* FAT32 directory entry (32 bytes) */
struct fat_dir_entry {
  uint8_t name[11];
  uint8_t attr;
  uint8_t nt_res;
  uint8_t crt_time_tenth;
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
uint32_t fat32_bytes_per_cluster(void);

/* Core operations */
int fat32_init(struct block_partition *part);
struct block_partition *fat32_partition(void);
void fat32_dump_cache_stats(void);
uint32_t fat32_walk_chain(uint32_t start_cluster, uint64_t page_index);
/* Walk the chain to the cluster at cluster_index, resuming from ip->walk_cursor
 * when possible (forward-only). Advances the cursor; returns the cluster or an
 * EOF marker (<2 / >=0x0FFFFFF8). */
uint32_t fat32_walk_chain_cached(struct inode *ip, uint64_t cluster_index,
                                 enum fat32_walk_source source);
void fat32_get_stats(struct fat32_stats *out);
void fat32_account_mapped_sector(enum fat32_walk_source source);

/* File operations */
int fat32_read(struct inode *ip, uint64_t offset, void *buf, size_t count);
int fat32_write(struct inode *ip, uint64_t offset, const void *buf,
                size_t count);
int fat32_ftruncate(struct inode *ip, uint64_t len);
int fat32_mkdir(const char *path);
int fat32_unlink(const char *path);
int fat32_rmdir(const char *path);
int fat32_stat(const char *path, void *stat_buf);
int fat32_getdents(uint32_t dir_cluster, uint64_t *pos, void *buf, size_t len);

extern struct fstype fat32_fstype;

#endif
