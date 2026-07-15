/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_FAT32_H
#define KERNEL_FAT32_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/bsd/mount.h"

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
int fat32_init(void);
uint32_t fat32_walk_chain(uint32_t start_cluster, uint64_t page_index);

/* File operations */
struct inode;
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
