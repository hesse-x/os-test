/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* kernel/fat32.c — In-kernel FAT32 filesystem (synchronous)
 * Ported from driver/fs_driver.cc, converting async state machines
 * to simple synchronous loops using blk_read/blk_write.
 *
 * Kernel constraint: C only (commit 18c91ca).
 */
#include "kernel/bsd/fat32.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/spinlock.h"

#include <xos/dirent.h>
#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/stat.h>

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

/* Global FAT lock: protects FAT modifications (free cluster scan + FAT entry
 * writes). Lock order: i_lock -> fat_lock -> ahci_lock (via blk_write) */
static spinlock fat_lock = SPINLOCK_INIT;

/* ==================== FAT sector cache (16 slots, LRU) ==================== */
#define FAT_CACHE_PAGES 16

struct fat_cache_entry {
  uint32_t sector_lba;
  uint8_t data[512];
};

static struct fat_cache_entry fat_cache[FAT_CACHE_PAGES];
static uint32_t fat_cache_time = 0;
static uint32_t fat_cache_age[FAT_CACHE_PAGES];
static spinlock fat_cache_lock = SPINLOCK_INIT;

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
    if (fat_cache_age[i] < fat_cache_age[best])
      best = i;
  }
  fat_cache[best].sector_lba = sector_lba;
  fat_cache_age[best] = ++fat_cache_time;
  spin_unlock(&fat_cache_lock);
  return best;
}

/* Read FAT sector into cache, returns cache slot */
static int fat_cache_read(uint32_t sector_lba) {
  int slot = fat_cache_lookup(sector_lba);
  if (slot >= 0)
    return slot;
  slot = fat_cache_alloc(sector_lba);
  if (blk_read_sector(sector_lba, fat_cache[slot].data) != 0)
    return -1;
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
static uint32_t fat32_read_entry(uint32_t cluster) {
  uint32_t fat_offset = cluster * 4;
  uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
  uint32_t offset_in_sector = fat_offset % 512;

  int slot = fat_cache_read(fat_sector);
  if (slot < 0)
    return 0x0FFFFFFF;

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
  if (blk_read_sector(fat_sector, sector_buf) != 0)
    return -EIO;

  uint8_t *p = sector_buf + offset_in_sector;
  uint32_t old = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  uint32_t nv = (old & 0xF0000000) | (value & 0x0FFFFFFF);
  p[0] = nv & 0xFF;
  p[1] = (nv >> 8) & 0xFF;
  p[2] = (nv >> 16) & 0xFF;
  p[3] = (nv >> 24) & 0xFF;

  /* Write FAT1 */
  if (blk_write(fat_sector, 1, sector_buf) != 0)
    return -EIO;
  /* Write FAT2 */
  if (blk_write(fat_sector + spf32, 1, sector_buf) != 0)
    return -EIO;

  /* Invalidate cache for this sector so subsequent reads get fresh data */
  fat_cache_invalidate_sector(fat_sector);
  fat_cache_invalidate_sector(fat_sector + spf32);

  return 0;
}

/* ==================== FAT chain walk ==================== */

/* Walk FAT chain from start_cluster, return the cluster at page_index */
uint32_t fat32_walk_chain(uint32_t start_cluster, uint64_t page_index) {
  uint32_t c = start_cluster;
  uint64_t max_walk =
      page_index * 2 +
      1; // safety limit: chain should not be longer than 2x page_index
  for (uint64_t i = 0; i < page_index; i++) {
    if (c < 2 || c >= 0x0FFFFFF8)
      return c;
    if (i > max_walk) {
      WARN_ON(1);        // FAT chain loop detected
      return 0x0FFFFFF8; // treat as EOF to prevent infinite loop
    }
    uint32_t next = fat32_read_entry(c);
    if (next >= 0x0FFFFFF8)
      return next;
    c = next;
  }
  return c;
}

/* ==================== Cluster allocation ==================== */

/* Find a free cluster and mark it as EOF. Returns cluster number or 0 on
 * failure. */
static uint32_t fat32_allocate_cluster(void) {
  for (uint32_t sector = 0; sector < spf32; sector++) {
    uint32_t abs_sector = ((next_free_hint / 128) + sector) % spf32;
    int slot = fat_cache_read(fat_start_lba + abs_sector);
    if (slot < 0)
      continue;

    uint8_t *fd = fat_cache[slot].data;
    for (int i = 0; i < 128; i++) {
      uint32_t c = abs_sector * 128 + i;
      if (c < 2 || c >= total_data_clusters + 2)
        continue;
      uint32_t e = (uint32_t)fd[i * 4] | ((uint32_t)fd[i * 4 + 1] << 8) |
                   ((uint32_t)fd[i * 4 + 2] << 16) |
                   ((uint32_t)fd[i * 4 + 3] << 24);
      e &= 0x0FFFFFFF;
      if (e == 0) {
        /* Found free cluster — mark as EOF in FAT */
        next_free_hint = c + 1;
        if (next_free_hint >= total_data_clusters + 2)
          next_free_hint = 2;
        if (fat32_write_fat_entry(c, 0x0FFFFFFF) != 0)
          return 0;
        return c;
      }
    }
  }
  return 0; /* ENOSPC */
}

/* Free an entire cluster chain starting from start_cluster */
static void fat32_free_chain(uint32_t start_cluster) {
  uint32_t c = start_cluster;
  int max_free = 1024; // safety limit: max clusters in a single chain
  while (c >= 2 && c < 0x0FFFFFF8) {
    if (--max_free <= 0) {
      WARN_ON(1); // FAT chain loop detected
      break;
    }
    uint32_t next = fat32_read_entry(c);
    fat32_write_fat_entry(c, 0);
    if (c < next_free_hint)
      next_free_hint = c;
    if (next >= 0x0FFFFFF8)
      break;
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
    for (int i = 1; i < 11; i++)
      out[i] = ' ';
    return;
  }
  if (user_len == 2 && user[0] == '.' && user[1] == '.') {
    out[0] = '.';
    out[1] = '.';
    for (int i = 2; i < 11; i++)
      out[i] = ' ';
    return;
  }
  for (int i = 0; i < 11; i++)
    out[i] = ' ';
  int i = 0, j = 0;
  while (i < user_len && user[i] != '.' && j < 8) {
    char c = user[i];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    out[j++] = c;
    i++;
  }
  if (i < user_len && user[i] == '.') {
    i++;
    j = 8;
    while (i < user_len && j < 11) {
      char c = user[i];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      out[j++] = c;
      i++;
    }
  }
}

static int match_83_name(const uint8_t stored[11], const char *name,
                         int name_len) {
  uint8_t expanded[11];
  format_83_name(name, name_len, expanded);
  for (int i = 0; i < 11; i++) {
    if (stored[i] != expanded[i])
      return 0;
  }
  return 1;
}

/* ==================== LFN helpers ==================== */

static int collect_lfn_entry(const struct fat_dir_entry *de, char *lfn_buf) {
  const uint8_t *raw = (const uint8_t *)de;
  int seq = raw[0] & 0x3F;

  static const int offsets[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
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
      for (int k = base + c + 1; k < 256; k++)
        lfn_buf[k] = '\0';
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
    if (lc == '\0')
      return 0;
    if (lc >= 'a' && lc <= 'z')
      lc -= 32;
    if (nc >= 'a' && nc <= 'z')
      nc -= 32;
    if (lc != nc)
      return 0;
  }
  return lfn_buf[name_len] == '\0';
}

/* ==================== Volume geometry accessors ==================== */

uint32_t fat32_data_start_lba(void) { return data_start_lba; }
uint32_t fat32_sectors_per_cluster(void) { return sectors_per_cluster; }
uint32_t fat32_bytes_per_cluster(void) { return bytes_per_cluster; }

/* ==================== Inode numbering ====================
 *
 * FAT32 has no on-disk inode number. We previously used the file's
 * start_cluster as the ino, but a freshly created empty file has
 * start_cluster == 0, so every empty file mapped to ino 0 and shared a
 * single inode cache entry (data corruption), and it also collided with
 * devtmpfs directory inodes that used ino 0. Use the stable, unique
 * location of the file's directory entry instead: the entry's directory
 * cluster and its index within that cluster. This is unique even for
 * empty files (their dir entry exists before any cluster is allocated).
 *
 * Layout: ino = dir_cluster * entries_per_cluster + dir_entry_index,
 * which stays below the devtmpfs range (0x80000000+) for any realistic
 * disk. The root directory (dir_idx < 0) keeps ino = root_cluster.
 */
static uint32_t fat32_make_ino(uint32_t dir_cluster, int dir_entry_idx) {
  if (dir_entry_idx < 0)
    return root_cluster;
  return dir_cluster * (bytes_per_cluster / 32) + (uint32_t)dir_entry_idx;
}

/* 前置声明:i_op 表在 R2-10 定义;簇读写 helper 定义在本文件后段。 */
static const struct inode_operations fat32_dir_iop;
static const struct inode_operations fat32_file_iop;
static int fat32_dir_rename(struct inode *old_parent, const char *old_name,
                            struct inode *new_parent, const char *new_name);
static uint8_t *read_cluster_buf(uint32_t cluster);
static int write_cluster_sector(uint32_t cluster, int sector_idx,
                                const uint8_t *data);

/* fat32_iget:经 inode_get_or_create 取/建 inode 并挂 i_op(唯一出口)。
 * cache 命中复用同 ino(幂等赋值 i_op 同值无竞态);miss 新建。
 * fat32 inode 无 fs 内部强引用,可被回收,故用 inode_get_or_create。 */
static struct inode *fat32_iget(uint32_t ino, int type, uint64_t size,
                                uint32_t cluster, uint32_t dir_cluster,
                                int dir_idx) {
  struct inode *ip =
      inode_get_or_create(ino, type, size, cluster, dir_cluster, dir_idx);
  if (!ip)
    return NULL;
  ip->i_op = (type == INODE_DIR) ? &fat32_dir_iop : &fat32_file_iop;
  return ip;
}

/* fat32_lookup_in_dir:在目录簇 dir_cluster 的整条 FAT 链上扫描名为 name
 *  的目录项,返回 0 命中(填充 out_*)/-ENOENT 未找到/-EIO。抽取自
 *  fat32_resolve_path 的内联扫描循环(§6.5 唯一目录扫描原语)。 */
static int fat32_lookup_in_dir(uint32_t dir_cluster, const char *name,
                               uint32_t *out_cluster, uint32_t *out_dir_cluster,
                               int *out_dir_idx, uint64_t *out_size,
                               int *out_is_dir) {
  int namelen = 0;
  while (name[namelen])
    namelen++;
  uint32_t scan = dir_cluster;
  char lfn_buf[256];
  __memset(lfn_buf, 0, sizeof(lfn_buf));
  while (scan >= 2 && scan < 0x0FFFFFF8) {
    uint32_t lba = data_start_lba + (scan - 2) * sectors_per_cluster;
    uint8_t *buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!buf)
      return -ENOMEM;
    if (blk_read(lba, sectors_per_cluster, buf) != 0) {
      kfree(buf);
      return -EIO;
    }
    int entries = bytes_per_cluster / 32;
    for (int i = 0; i < entries; i++) {
      struct fat_dir_entry *de = (struct fat_dir_entry *)(buf + i * 32);
      if (de->name[0] == 0x00) {
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
      int matched = 0;
      if (lfn_buf[0] != '\0')
        matched = match_lfn_name(lfn_buf, name, namelen);
      if (!matched)
        matched = match_83_name(de->name, name, namelen);
      lfn_buf[0] = '\0';
      if (matched) {
        uint32_t ec = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
        if (ec == 0 && (de->attr & 0x10))
          ec = root_cluster;
        *out_cluster = ec;
        *out_dir_cluster = scan;
        *out_dir_idx = i;
        *out_size = de->file_size;
        *out_is_dir = (de->attr & 0x10) ? 1 : 0;
        kfree(buf);
        return 0;
      }
    }
    kfree(buf);
    uint32_t next = fat32_read_entry(scan);
    if (next >= 0x0FFFFFF8)
      return -ENOENT;
    scan = next;
  }
  return -ENOENT;
}

/* fat32_dir_lookup:在目录 inode dir 内查名为 name 的直接子项,返 +1 inode 或
 * NULL。 */
static struct inode *fat32_dir_lookup(struct inode *dir, const char *name) {
  uint32_t cluster, dir_cluster;
  int dir_idx, is_dir;
  uint64_t size;
  int rc = fat32_lookup_in_dir(dir->start_cluster, name, &cluster, &dir_cluster,
                               &dir_idx, &size, &is_dir);
  if (rc != 0)
    return NULL;
  int type = is_dir ? INODE_DIR : INODE_REGULAR;
  uint32_t ino = fat32_make_ino(dir_cluster, dir_idx);
  return fat32_iget(ino, type, size, cluster, dir_cluster, dir_idx);
}

/* fat32_dir_create:在目录 dir 内建普通文件 name,返 +1 新 inode 或
 * ERR_PTR(-errno)。 */
static struct inode *fat32_dir_create(struct inode *dir, const char *name,
                                      int mode) {
  (void)mode;
  int namelen = 0;
  while (name[namelen])
    namelen++;
  if (namelen == 0)
    return ERR_PTR(-ENOENT);

  spin_lock(&fat_lock);
  /* 在 dir->start_cluster 的簇链上找空闲槽(0x00 或 0xE5),不足则扩展一簇。 */
  int entries = bytes_per_cluster / 32;
  uint32_t target_cluster = dir->start_cluster;
  int free_idx = -1;
  int was_end_of_dir = 0;
  uint32_t tail = dir->start_cluster;
  uint32_t cur = dir->start_cluster;
  while (cur >= 2 && cur < 0x0FFFFFF8) {
    tail = cur;
    uint8_t *db = read_cluster_buf(cur);
    if (!db) {
      spin_unlock(&fat_lock);
      return ERR_PTR(-EIO);
    }
    for (int i = 0; i < entries; i++) {
      struct fat_dir_entry *de = (struct fat_dir_entry *)(db + i * 32);
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
    kfree(db);
    if (free_idx >= 0) {
      target_cluster = cur;
      break;
    }
    cur = fat32_read_entry(cur);
  }
  if (free_idx < 0) {
    uint32_t nc = fat32_allocate_cluster();
    if (nc == 0) {
      spin_unlock(&fat_lock);
      return ERR_PTR(-ENOSPC);
    }
    if (fat32_link_cluster(tail, nc) != 0) {
      fat32_write_fat_entry(nc, 0);
      spin_unlock(&fat_lock);
      return ERR_PTR(-EIO);
    }
    uint8_t *zb = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!zb) {
      fat32_write_fat_entry(nc, 0);
      fat32_write_fat_entry(tail, 0x0FFFFFFF);
      spin_unlock(&fat_lock);
      return ERR_PTR(-ENOMEM);
    }
    __memset(zb, 0, bytes_per_cluster);
    uint32_t lba = data_start_lba + (nc - 2) * sectors_per_cluster;
    blk_write(lba, sectors_per_cluster, zb);
    kfree(zb);
    target_cluster = nc;
    free_idx = 0;
    was_end_of_dir = 1;
  }
  uint8_t *db = read_cluster_buf(target_cluster);
  if (!db) {
    spin_unlock(&fat_lock);
    return ERR_PTR(-EIO);
  }
  struct fat_dir_entry ne;
  __memset(&ne, 0, sizeof(ne));
  format_83_name(name, namelen, ne.name);
  ne.attr = 0;
  ne.fst_clus_hi = 0;
  ne.fst_clus_lo = 0;
  ne.file_size = 0;
  __memcpy(db + free_idx * 32, &ne, 32);
  if (was_end_of_dir && free_idx + 1 < entries)
    __memset(db + (free_idx + 1) * 32, 0, 32);
  int sec = (free_idx * 32) / 512;
  int wrc = write_cluster_sector(target_cluster, sec, db + sec * 512);
  if (wrc == 0 && was_end_of_dir && free_idx + 1 < entries) {
    int ns = ((free_idx + 1) * 32) / 512;
    if (ns != sec)
      wrc = write_cluster_sector(target_cluster, ns, db + ns * 512);
  }
  kfree(db);
  spin_unlock(&fat_lock);
  if (wrc != 0)
    return ERR_PTR(-EIO);
  uint32_t ino = fat32_make_ino(target_cluster, free_idx);
  return fat32_iget(ino, INODE_REGULAR, 0, 0, target_cluster, free_idx);
}

/* fat32_dir_mkdir:在 dir 内建子目录 name,返 0 成功 / -errno 失败。 */
static int fat32_dir_mkdir(struct inode *dir, const char *name, int mode) {
  (void)mode;
  int namelen = 0;
  while (name[namelen])
    namelen++;
  if (namelen == 0)
    return -ENOENT;
  spin_lock(&fat_lock);
  uint32_t new_cluster = fat32_allocate_cluster();
  if (new_cluster == 0) {
    spin_unlock(&fat_lock);
    return -ENOSPC;
  }
  uint8_t *db = (uint8_t *)kmalloc(bytes_per_cluster);
  if (!db) {
    fat32_write_fat_entry(new_cluster, 0);
    spin_unlock(&fat_lock);
    return -ENOMEM;
  }
  __memset(db, 0, bytes_per_cluster);
  struct fat_dir_entry *dot = (struct fat_dir_entry *)db;
  __memset(dot, 0, 32);
  dot->name[0] = '.';
  for (int i = 1; i < 11; i++)
    dot->name[i] = ' ';
  dot->attr = 0x10;
  dot->fst_clus_hi = (new_cluster >> 16) & 0xFFFF;
  dot->fst_clus_lo = new_cluster & 0xFFFF;
  struct fat_dir_entry *dd = (struct fat_dir_entry *)(db + 32);
  __memset(dd, 0, 32);
  dd->name[0] = '.';
  dd->name[1] = '.';
  for (int i = 2; i < 11; i++)
    dd->name[i] = ' ';
  dd->attr = 0x10;
  dd->fst_clus_hi = (dir->start_cluster >> 16) & 0xFFFF;
  dd->fst_clus_lo = dir->start_cluster & 0xFFFF;
  uint32_t lba = data_start_lba + (new_cluster - 2) * sectors_per_cluster;
  blk_write(lba, sectors_per_cluster, db);
  kfree(db);
  /* 在父目录 dir->start_cluster 找空闲槽写新目录项 */
  uint8_t *pb = read_cluster_buf(dir->start_cluster);
  if (!pb) {
    fat32_free_chain(new_cluster);
    spin_unlock(&fat_lock);
    return -EIO;
  }
  int entries = bytes_per_cluster / 32;
  int free_idx = -1;
  for (int i = 0; i < entries; i++) {
    struct fat_dir_entry *de = (struct fat_dir_entry *)(pb + i * 32);
    if (de->name[0] == 0x00 || de->name[0] == 0xE5) {
      free_idx = i;
      break;
    }
  }
  if (free_idx < 0) {
    kfree(pb);
    fat32_free_chain(new_cluster);
    spin_unlock(&fat_lock);
    return -ENOSPC;
  }
  struct fat_dir_entry ne;
  __memset(&ne, 0, sizeof(ne));
  format_83_name(name, namelen, ne.name);
  ne.attr = 0x10;
  ne.fst_clus_hi = (new_cluster >> 16) & 0xFFFF;
  ne.fst_clus_lo = new_cluster & 0xFFFF;
  ne.file_size = 0;
  __memcpy(pb + free_idx * 32, &ne, 32);
  int sec = (free_idx * 32) / 512;
  int wrc = write_cluster_sector(dir->start_cluster, sec, pb + sec * 512);
  kfree(pb);
  spin_unlock(&fat_lock);
  if (wrc != 0)
    return -EIO;
  /* 预建 inode cache 条目(调用者 sys_mkdir 不取回 inode,仅判定成败)。 */
  uint32_t ino = fat32_make_ino(dir->start_cluster, free_idx);
  struct inode *ip =
      fat32_iget(ino, INODE_DIR, 0, new_cluster, dir->start_cluster, free_idx);
  if (ip)
    inode_put(ip); /* cache 持基准 ref,此处还掉 iget 的 +1 */
  return 0;
}

/* fat32_dir_unlink:从 dir 删除子文件 name。 */
static int fat32_dir_unlink(struct inode *dir, const char *name) {
  uint32_t cluster, dir_cluster;
  int dir_idx, is_dir;
  uint64_t size;
  (void)size;
  int rc = fat32_lookup_in_dir(dir->start_cluster, name, &cluster, &dir_cluster,
                               &dir_idx, &size, &is_dir);
  if (rc != 0)
    return rc;
  uint8_t *db = read_cluster_buf(dir_cluster);
  if (!db)
    return -EIO;
  struct fat_dir_entry *de = (struct fat_dir_entry *)(db + dir_idx * 32);
  if (de->attr & 0x10) {
    kfree(db);
    return -EISDIR;
  }
  uint32_t target_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
  struct inode *ip = inode_lookup(fat32_make_ino(dir_cluster, dir_idx));
  if (ip) {
    page_cache_invalidate_inode(ip);
    inode_put(ip);
  }
  db[dir_idx * 32] = 0xE5;
  int sec = (dir_idx * 32) / 512;
  write_cluster_sector(dir_cluster, sec, db + sec * 512);
  kfree(db);
  if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8) {
    spin_lock(&fat_lock);
    fat32_free_chain(target_cluster);
    spin_unlock(&fat_lock);
  }
  return 0;
}

/* fat32_dir_is_empty:扫目录簇链,仅含 . / .. 或全 0x00 视为空,返 1;
 *  否则 0。抽自 fat32_dir_rmdir 的内联验空段,供 rmdir/rename 共用。 */
static int fat32_dir_is_empty(uint32_t cluster) {
  uint32_t cc = cluster;
  while (cc >= 2 && cc < 0x0FFFFFF8) {
    uint8_t *dbuf = read_cluster_buf(cc);
    if (!dbuf)
      return 0;
    int entries = bytes_per_cluster / 32;
    for (int i = 0; i < entries; i++) {
      struct fat_dir_entry *d = (struct fat_dir_entry *)(dbuf + i * 32);
      if (d->name[0] == 0x00) {
        kfree(dbuf);
        return 1; /* 目录尾,之前只见 . .. → 空 */
      }
      if (d->name[0] == 0xE5)
        continue;
      if (d->name[0] == '.' && (d->name[1] == ' ' || d->name[1] == '.'))
        continue;
      if (d->name[0] == '.' && d->name[1] == '.')
        continue;
      kfree(dbuf);
      return 0;
    }
    kfree(dbuf);
    uint32_t next = fat32_read_entry(cc);
    if (next >= 0x0FFFFFF8)
      break;
    cc = next;
  }
  return 1;
}

/* fat32_dir_rmdir:从 dir 删除空子目录 name。 */
static int fat32_dir_rmdir(struct inode *dir, const char *name) {
  uint32_t cluster, dir_cluster;
  int dir_idx, is_dir;
  uint64_t size;
  int rc = fat32_lookup_in_dir(dir->start_cluster, name, &cluster, &dir_cluster,
                               &dir_idx, &size, &is_dir);
  if (rc != 0)
    return rc;
  if (!is_dir)
    return -ENOTDIR;
  uint32_t target_cluster = cluster;
  if (target_cluster == 0)
    target_cluster = root_cluster;
  if (!fat32_dir_is_empty(target_cluster))
    return -EBUSY;
  spin_lock(&fat_lock);
  uint8_t *db = read_cluster_buf(dir_cluster);
  if (!db) {
    spin_unlock(&fat_lock);
    return -EIO;
  }
  db[dir_idx * 32] = 0xE5;
  int sec = (dir_idx * 32) / 512;
  write_cluster_sector(dir_cluster, sec, db + sec * 512);
  kfree(db);
  if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8)
    fat32_free_chain(target_cluster);
  spin_unlock(&fat_lock);
  return 0;
}

/* fat32_getattr:从 inode 字段填 kstat。S08: 报真实 ip->mode/uid/gid(创建时
 * 由 sys_open/mkdir 设),blksize=fat32 簇大小。FAT32 磁盘不存权限位,mode/uid/gid
 * 仅内存态(reboot 后回默认);本 OS 单 root,uid 多为 0,可接受(记 todo 真权限
 * FS)。 */
static int fat32_getattr(struct inode *ip, struct kstat *ks) {
  __memset(ks, 0, sizeof(*ks));
  ks->st_ino = ip->ino;
  ks->st_mode = ip->mode;
  ks->st_uid = ip->uid;
  ks->st_gid = ip->gid;
  ks->st_nlink = 1;
  ks->st_size = (int64_t)ip->size;
  ks->st_blksize = (int64_t)fat32_bytes_per_cluster();
  ks->st_blocks = (ip->size + 511) / 512;
  return 0;
}

/* fat32_setattr:改 inode 大小。锁序 i_lock -> fat_lock(§6.6):setattr 内部
 *  自取 i_lock 再调 fat32_ftruncate(内部取 fat_lock)。调用者不持 i_lock。 */
static int fat32_setattr(struct inode *ip, uint64_t size) {
  spin_lock(&ip->i_lock);
  int rc = fat32_ftruncate(ip, size);
  spin_unlock(&ip->i_lock);
  return rc;
}

/* fat32_dir_rename:将 old_parent 下 old_name 目录项改到 new_parent 下
 * new_name。 实现策略:在 new_parent 找目标槽(若 new
 * 已存在则就地覆盖其槽,否则找空闲 槽/扩簇),把 old 目录项 32B
 * 原样复制到目标槽并仅重写 name 为 new_name 的 8.3 形式(attr/fst_clus/size 不变
 * → inode 身份即首簇不变),再标 old 旧槽 0xE5;覆盖时释放 new 原簇链。锁序对齐
 * fat32_dir_create:fat_lock 全程持有。
 *
 *  POSIX 语义(对齐 Linux rename(2)):
 *   - old 不存在 → -ENOENT
 *   - new 不存在 → 纯改名(目录项迁移,簇链不变)
 *   - new 存在且为文件、old 亦文件 → 覆盖(释放 new 簇链)
 *   - new 存在且为空目录、old 亦目录 → 覆盖(释放 new 目录簇链)
 *   - new 存在且非空目录 → -ENOTEMPTY
 *   - old 目录、new 文件 → -EISDIR;old 文件、new 目录 → -ENOTDIR
 *   - old==new 同名 → 0 no-op
 *   - 同一 inode(old/new 指向同首簇) → 0 no-op(防自覆盖)
 *  跨目录目录移动需更新被移目录 .. 与防环,本实现暂不支持:old_parent !=
 *  new_parent 且 old 为目录 → -EXDEV。同目录改名 .. 不变,天然无环。 */
static int fat32_dir_rename(struct inode *old_parent, const char *old_name,
                            struct inode *new_parent, const char *new_name) {
  if (!old_parent || !old_name || !new_parent || !new_name)
    return -EFAULT;

  /* old/new 同目录同名 → no-op(对齐 Linux)。 */
  if (old_parent == new_parent && __strcmp(old_name, new_name) == 0)
    return 0;

  /* 跨目录移动目录需 .. 更新 + 防环,本实现不支持(对齐 mvfat:返 -EXDEV)。
   * 同目录改名 .. 不变;跨目录文件移动无 .. 需求亦不支持以保边界清晰。 */
  if (old_parent != new_parent)
    return -EXDEV;

  int namelen = 0;
  while (new_name[namelen])
    namelen++;
  if (namelen == 0)
    return -ENOENT;

  uint32_t old_cluster, old_dir_cluster;
  int old_dir_idx, old_is_dir;
  uint64_t old_size;
  (void)old_size;
  int rc = fat32_lookup_in_dir(old_parent->start_cluster, old_name,
                               &old_cluster, &old_dir_cluster, &old_dir_idx,
                               &old_size, &old_is_dir);
  if (rc != 0)
    return rc; /* -ENOENT */

  uint32_t new_cluster, new_dir_cluster;
  int new_dir_idx, new_is_dir;
  uint64_t new_size;
  (void)new_size;
  rc = fat32_lookup_in_dir(new_parent->start_cluster, new_name, &new_cluster,
                           &new_dir_cluster, &new_dir_idx, &new_size,
                           &new_is_dir);
  int new_exists = (rc == 0);
  if (rc != 0 && rc != -ENOENT)
    return rc; /* -EIO 等 */

  /* old/new 指向同一首簇 → 同一 inode,对齐 Linux 返 0(防"覆盖自身"误删)。 */
  if (new_exists && old_cluster == new_cluster && old_cluster >= 2 &&
      old_cluster < 0x0FFFFFF8)
    return 0;

  /* 覆盖语义边界校验(new 存在时)。 */
  if (new_exists) {
    if (old_is_dir) {
      if (!new_is_dir)
        return -EISDIR; /* old 目录 → new 文件 */
      uint32_t new_dir_cluster0 = new_cluster ? new_cluster : root_cluster;
      if (!fat32_dir_is_empty(new_dir_cluster0))
        return -ENOTEMPTY;
    } else {
      if (new_is_dir)
        return -ENOTDIR; /* old 文件 → new 目录 */
    }
  }

  spin_lock(&fat_lock);

  /* 读 old 目录项(保留 attr/fst_clus/size,仅重写 name 为 new_name 8.3)。 */
  uint8_t *old_db = read_cluster_buf(old_dir_cluster);
  if (!old_db) {
    spin_unlock(&fat_lock);
    return -EIO;
  }
  struct fat_dir_entry ne;
  __memcpy(&ne, old_db + old_dir_idx * 32, sizeof(ne));
  kfree(old_db);
  format_83_name(new_name, namelen, ne.name);

  /* 确定目标槽:覆盖用 new 的旧槽;纯改名找空闲槽(0x00/0xE5),不足则扩簇。 */
  uint32_t target_cluster;
  int target_idx;
  int was_end_of_dir = 0;
  if (new_exists) {
    target_cluster = new_dir_cluster;
    target_idx = new_dir_idx;
  } else {
    int entries = bytes_per_cluster / 32;
    target_cluster = new_parent->start_cluster;
    target_idx = -1;
    uint32_t tail = new_parent->start_cluster;
    uint32_t cur = new_parent->start_cluster;
    while (cur >= 2 && cur < 0x0FFFFFF8) {
      tail = cur;
      uint8_t *db = read_cluster_buf(cur);
      if (!db) {
        spin_unlock(&fat_lock);
        return -EIO;
      }
      for (int i = 0; i < entries; i++) {
        struct fat_dir_entry *de = (struct fat_dir_entry *)(db + i * 32);
        if (de->name[0] == 0x00) {
          target_idx = i;
          was_end_of_dir = 1;
          break;
        }
        if (de->name[0] == 0xE5) {
          target_idx = i;
          break;
        }
      }
      kfree(db);
      if (target_idx >= 0) {
        target_cluster = cur;
        break;
      }
      cur = fat32_read_entry(cur);
    }
    if (target_idx < 0) {
      uint32_t nc = fat32_allocate_cluster();
      if (nc == 0) {
        spin_unlock(&fat_lock);
        return -ENOSPC;
      }
      if (fat32_link_cluster(tail, nc) != 0) {
        fat32_write_fat_entry(nc, 0);
        spin_unlock(&fat_lock);
        return -EIO;
      }
      uint8_t *zb = (uint8_t *)kmalloc(bytes_per_cluster);
      if (!zb) {
        fat32_write_fat_entry(nc, 0);
        fat32_write_fat_entry(tail, 0x0FFFFFFF);
        spin_unlock(&fat_lock);
        return -ENOMEM;
      }
      __memset(zb, 0, bytes_per_cluster);
      uint32_t lba = data_start_lba + (nc - 2) * sectors_per_cluster;
      blk_write(lba, sectors_per_cluster, zb);
      kfree(zb);
      target_cluster = nc;
      target_idx = 0;
      was_end_of_dir = 1;
    }
  }

  /* 写目标槽:复制 old 目录项(已改 name)。维持 end-of-dir 标记。 */
  uint8_t *new_db = read_cluster_buf(target_cluster);
  if (!new_db) {
    spin_unlock(&fat_lock);
    return -EIO;
  }
  int entries = bytes_per_cluster / 32;
  __memcpy(new_db + target_idx * 32, &ne, 32);
  if (was_end_of_dir && target_idx + 1 < entries)
    __memset(new_db + (target_idx + 1) * 32, 0, 32);
  int sec = (target_idx * 32) / 512;
  int wrc = write_cluster_sector(target_cluster, sec, new_db + sec * 512);
  if (wrc == 0 && was_end_of_dir && target_idx + 1 < entries) {
    int ns = ((target_idx + 1) * 32) / 512;
    if (ns != sec)
      wrc = write_cluster_sector(target_cluster, ns, new_db + ns * 512);
  }
  kfree(new_db);
  if (wrc != 0) {
    spin_unlock(&fat_lock);
    return -EIO;
  }

  /* 删 old 旧槽(标 0xE5)。覆盖场景下 old 槽 ≠ new 槽(异名),独立标记。 */
  if (old_dir_cluster != target_cluster || old_dir_idx != target_idx) {
    uint8_t *odb = read_cluster_buf(old_dir_cluster);
    if (!odb) {
      spin_unlock(&fat_lock);
      return -EIO;
    }
    odb[old_dir_idx * 32] = 0xE5;
    int osec = (old_dir_idx * 32) / 512;
    wrc = write_cluster_sector(old_dir_cluster, osec, odb + osec * 512);
    kfree(odb);
    if (wrc != 0) {
      spin_unlock(&fat_lock);
      return -EIO;
    }
  }

  /* 覆盖:释放 new 原簇链。FAT32 无 nlink,簇链释放即数据删除。 */
  if (new_exists) {
    uint32_t nc = new_cluster;
    if (nc >= 2 && nc < 0x0FFFFFF8)
      fat32_free_chain(nc);
  }

  /* inode cache 同步:old inode 的 ino 随目录项位置变化,但其内容(首簇/size)
   * 不变。照 unlink 失效其 page cache;已打开 fd 仍走首簇寻址,不受目录项位置
   * 影响(FAT32 文件 I/O 经首簇,不依赖 ino 的 dir_idx)。不主动迁移 inode cache
   * 条目;下次 lookup 按新位置 fat32_iget 建新 inode,旧 inode 关闭时回收。 */
  struct inode *old_ip =
      inode_lookup(fat32_make_ino(old_dir_cluster, old_dir_idx));
  if (old_ip) {
    page_cache_invalidate_inode(old_ip);
    inode_put(old_ip);
  }

  spin_unlock(&fat_lock);
  return 0;
}

static const struct inode_operations fat32_dir_iop = {
    .lookup = fat32_dir_lookup,
    .create = fat32_dir_create,
    .mkdir = fat32_dir_mkdir,
    .unlink = fat32_dir_unlink,
    .rmdir = fat32_dir_rmdir,
    .rename = fat32_dir_rename,
    .getattr = fat32_getattr,
    .setattr = fat32_setattr,
};

static const struct inode_operations fat32_file_iop = {
    .getattr = fat32_getattr,
    .setattr = fat32_setattr,
};

/* fat32_mount_root:返回挂载点根 inode(已 inode_get,+1)。根 ino=root_cluster。
 *  经 fat32_iget 出口挂 fat32_dir_iop——修复 R1 stub 漏挂 i_op 导致的启动死锁。
 */
static struct inode *fat32_mount_root(struct mount_entry *m) {
  (void)m;
  return fat32_iget(root_cluster, INODE_DIR, 0, root_cluster, root_cluster, -1);
}

/* ==================== Path resolution (synchronous) ==================== */

static int fat32_resolve_path(const char *path, uint32_t *out_cluster,
                              uint32_t *out_dir_cluster, int *out_dir_entry_idx,
                              uint64_t *out_file_size, int is_parent) {
  if (!path || path[0] != '/')
    return -ENOENT;

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
    while (path[path_len])
      path_len++;
    for (int i = path_len - 1; i >= 0; i--) {
      if (path[i] == '/') {
        parent_end = i;
        break;
      }
    }
    if (parent_end < 0)
      return -ENOENT;
    if (parent_end == 0) {
      /* Parent is root */
      *out_cluster = root_cluster;
      *out_dir_cluster = root_cluster;
      *out_dir_entry_idx = -1;
      *out_file_size = 0;
      return 0;
    }
    /* Extract leaf name length after last slash (not stored, just advance past
     * it) */
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
    while (*p && *p != '/' && clen < 255)
      component[clen++] = *p++;
    component[clen] = '\0';
    if (*p == '/')
      p++;

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
          if (!buf2)
            return -ENOMEM;
          if (blk_read(lba2, sectors_per_cluster, buf2) != 0) {
            kfree(buf2);
            return -EIO;
          }
          int entries2 = bytes_per_cluster / 32;
          for (int j = 0; j < entries2; j++) {
            struct fat_dir_entry *de2 = (struct fat_dir_entry *)(buf2 + j * 32);
            if (de2->name[0] == 0x00)
              break;
            if (de2->name[0] == 0xE5)
              continue;
            if (de2->attr == 0x0F)
              continue;
            if (de2->name[0] == '.' && de2->name[1] == '.') {
              uint32_t pc =
                  ((uint32_t)de2->fst_clus_hi << 16) | de2->fst_clus_lo;
              if (pc == 0)
                pc = root_cluster;
              parent_cluster = pc;
              kfree(buf2);
              goto dotdot_done;
            }
          }
          kfree(buf2);
          scan2 = fat32_read_entry(scan2);
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
      if (!buf)
        return -ENOMEM;
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
          uint32_t entry_cluster =
              ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
          if (entry_cluster == 0 && (de->attr & 0x10))
            entry_cluster = root_cluster;

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
      if (found)
        break;

      /* Follow FAT chain */
      uint32_t next = fat32_read_entry(scan_cluster);
      if (next >= 0x0FFFFFF8)
        return -ENOENT;
      scan_cluster = next;
    }

    if (!found)
      return -ENOENT;
  }

  return -ENOENT;
}

/* ==================== Directory entry read/write helpers ====================
 */

/* Read a cluster from disk into a kmalloc'd buffer */
static uint8_t *read_cluster_buf(uint32_t cluster) {
  uint8_t *buf = (uint8_t *)kmalloc(bytes_per_cluster);
  if (!buf)
    return NULL;
  uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;
  if (blk_read(lba, sectors_per_cluster, buf) != 0) {
    kfree(buf);
    return NULL;
  }
  return buf;
}

/* Write back a single sector from a cluster buffer */
static int write_cluster_sector(uint32_t cluster, int sector_idx,
                                const uint8_t *data) {
  uint32_t lba =
      data_start_lba + (cluster - 2) * sectors_per_cluster + sector_idx;
  return blk_write(lba, 1, data);
}

/* Update directory entry on disk */
static int fat32_update_dir_entry(uint32_t dir_cluster, int dir_idx,
                                  uint32_t start_cluster, uint32_t file_size) {
  uint8_t *buf = read_cluster_buf(dir_cluster);
  if (!buf)
    return -EIO;

  struct fat_dir_entry *de = (struct fat_dir_entry *)(buf + dir_idx * 32);
  de->fst_clus_hi = (start_cluster >> 16) & 0xFFFF;
  de->fst_clus_lo = start_cluster & 0xFFFF;
  de->file_size = file_size;

  int sector_idx = (dir_idx * 32) / 512;
  int rc =
      write_cluster_sector(dir_cluster, sector_idx, buf + sector_idx * 512);
  kfree(buf);
  return rc;
}

/* ==================== Truncate (free cluster chain, zero size)
 * ==================== */

static int fat32_truncate(uint32_t cluster, uint32_t dir_cluster, int dir_idx) {
  spin_lock(&fat_lock);
  fat32_free_chain(cluster);
  spin_unlock(&fat_lock);

  /* Update directory entry: cluster=0, size=0 */
  return fat32_update_dir_entry(dir_cluster, dir_idx, 0, 0);
}

/* ==================== ftruncate to an arbitrary length ====================
 */
/* Grow or shrink an existing regular file to exactly len bytes. New bytes
 * (when growing) read back as zero. Updates the inode size + on-disk dir entry
 * and invalidates the page cache so subsequent reads see the new length.
 * Caller holds ip->i_lock. */
int fat32_ftruncate(struct inode *ip, uint64_t len) {
  if (ip->type != INODE_REGULAR)
    return -EINVAL;

  uint32_t bpc = fat32_bytes_per_cluster();
  if (bpc == 0)
    return -EIO;

  /* No work if size already matches. */
  if (ip->size == len)
    return 0;

  if (len == 0) {
    /* Shrink-to-zero reuses the simple path. */
    int rc = fat32_truncate(ip->start_cluster, ip->dir_start_cluster,
                            ip->dir_entry_index);
    if (rc)
      return rc;
    ip->start_cluster = 0;
    ip->size = 0;
    page_cache_invalidate_inode(ip);
    return 0;
  }

  uint32_t keep_clusters = (uint32_t)((len + bpc - 1) / bpc);

  if (len < ip->size) {
    /* Shrink: free every cluster past the keep boundary. */
    spin_lock(&fat_lock);
    if (ip->start_cluster == 0) {
      spin_unlock(&fat_lock);
      return -EIO;
    }
    /* Walk to the last cluster we keep (index keep_clusters-1). */
    uint32_t c = ip->start_cluster;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < keep_clusters; i++) {
      if (c < 2 || c >= 0x0FFFFFF8) {
        /* Chain shorter than keep_clusters — nothing to free. */
        c = 0;
        break;
      }
      prev = c;
      c = fat32_read_entry(c);
    }
    if (prev != 0 && c >= 2 && c < 0x0FFFFFF8) {
      /* Terminate the kept chain and free the rest. */
      fat32_write_fat_entry(prev, 0x0FFFFFFF);
      fat32_free_chain(c);
    }
    spin_unlock(&fat_lock);
  } else {
    /* Grow: allocate clusters until the chain holds keep_clusters. */
    spin_lock(&fat_lock);
    if (ip->start_cluster == 0) {
      uint32_t first = fat32_allocate_cluster();
      if (first == 0) {
        spin_unlock(&fat_lock);
        return -ENOSPC;
      }
      ip->start_cluster = first;
      /* Zero the new cluster on disk. */
      uint8_t z[4096];
      __memset(z, 0, bpc);
      uint32_t lba = data_start_lba + (first - 2) * sectors_per_cluster;
      blk_write(lba, sectors_per_cluster, z);
      keep_clusters -= 1;
    }
    /* Find current tail. */
    uint32_t tail = ip->start_cluster;
    uint32_t have = 1;
    int guard = 1024;
    while (guard-- > 0) {
      uint32_t next = fat32_read_entry(tail);
      if (next >= 0x0FFFFFF8)
        break;
      tail = next;
      have++;
    }
    /* Append (keep_clusters - have) new zeroed clusters. */
    while (have < keep_clusters) {
      uint32_t nc = fat32_allocate_cluster();
      if (nc == 0) {
        spin_unlock(&fat_lock);
        /* Partial grow: update size to what we have so far. */
        ip->size = len;
        fat32_update_dir_entry(ip->dir_start_cluster, ip->dir_entry_index,
                               ip->start_cluster, (uint32_t)ip->size);
        return -ENOSPC;
      }
      uint8_t z[4096];
      __memset(z, 0, bpc);
      uint32_t lba = data_start_lba + (nc - 2) * sectors_per_cluster;
      blk_write(lba, sectors_per_cluster, z);
      fat32_link_cluster(tail, nc);
      tail = nc;
      have++;
    }
    spin_unlock(&fat_lock);
  }

  ip->size = len;
  if (ip->dir_start_cluster >= 2) {
    fat32_update_dir_entry(ip->dir_start_cluster, ip->dir_entry_index,
                           ip->start_cluster, (uint32_t)ip->size);
  }
  page_cache_invalidate_inode(ip);
  return 0;
}

/* ==================== FAT32 init ==================== */

int fat32_init(void) {
  printk(LOG_INFO, "fat32_init: starting\n");

  /* Initialize FAT cache */
  for (int i = 0; i < FAT_CACHE_PAGES; i++) {
    fat_cache[i].sector_lba = 0xFFFFFFFF;
    fat_cache_age[i] = 0;
  }

  /* Read MBR (LBA 0) */
  uint8_t mbr[512];
  if (blk_read_sector(0, mbr) != 0) {
    printk(LOG_ERROR, "fat32_init: MBR read failed\n");
    return -EIO;
  }

  /* Scan partition table for FAT32 (type 0x0B or 0x0C) */
  part_start_lba = 0;
  uint32_t part_total_sectors = 0;
  for (int i = 0; i < 4; i++) {
    uint8_t *entry = mbr + 0x1BE + i * 16;
    uint8_t ptype = entry[4];
    if (ptype == 0x0B || ptype == 0x0C) {
      part_start_lba = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) |
                       ((uint32_t)entry[10] << 16) |
                       ((uint32_t)entry[11] << 24);
      part_total_sectors = (uint32_t)entry[12] | ((uint32_t)entry[13] << 8) |
                           ((uint32_t)entry[14] << 16) |
                           ((uint32_t)entry[15] << 24);
      break;
    }
  }

  /* Fallback: if no partition found, try LBA 2149 (current disk layout) */
  if (part_start_lba == 0) {
    printk(LOG_WARN,
           "fat32_init: no FAT32 partition in MBR, trying LBA 2149\n");
    part_start_lba = 2149;
  }

  /* Read BPB */
  uint8_t bpb[512];
  if (blk_read_sector(part_start_lba, bpb) != 0) {
    printk(LOG_ERROR, "fat32_init: BPB read failed\n");
    return -EIO;
  }

  uint16_t bps = (uint16_t)bpb[11] | ((uint16_t)bpb[12] << 8);
  sectors_per_cluster = bpb[13];
  uint16_t reserved = (uint16_t)bpb[14] | ((uint16_t)bpb[15] << 8);
  spf32 = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
          ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
  root_cluster = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                 ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);

  if (bps != 512 || sectors_per_cluster == 0 || spf32 == 0 ||
      root_cluster < 2) {
    printk(LOG_ERROR,
           "fat32_init: invalid BPB (bps=%u spc=%u spf=%u root=%u)\n", bps,
           sectors_per_cluster, spf32, root_cluster);
    return -EINVAL;
  }

  fat_start_lba = part_start_lba + reserved;
  data_start_lba = fat_start_lba + spf32 * 2;
  bytes_per_cluster = sectors_per_cluster * 512;

  if (part_total_sectors > 0) {
    uint32_t data_sectors =
        part_total_sectors - (data_start_lba - part_start_lba);
    total_data_clusters = data_sectors / sectors_per_cluster;
  } else {
    total_data_clusters = 0;
  }

  /* Pre-warm FAT cache */
  for (uint32_t s = 0; s < spf32; s++) {
    fat_cache_read(fat_start_lba + s);
  }

  printk(
      LOG_INFO,
      "fat32_init: part=%u fat=%u data=%u root=%u spc=%u bpc=%u total_cl=%u\n",
      part_start_lba, fat_start_lba, data_start_lba, root_cluster,
      sectors_per_cluster, bytes_per_cluster, total_data_clusters);
  return 0;
}

/* ==================== File read ==================== */

int fat32_read(struct inode *ip, uint64_t offset, void *buf, size_t count) {
  if (offset >= ip->size)
    return 0;
  uint64_t avail = ip->size - offset;
  if (count > avail)
    count = (size_t)avail;

  size_t nread = 0;
  while (nread < count) {
    uint64_t page_idx = (offset + nread) / 4096;
    uint32_t page_off = (offset + nread) % 4096;
    uint32_t chunk = 4096 - page_off;
    if (chunk > count - nread)
      chunk = count - nread;

    struct cache_page *cp = page_cache_fill(ip, page_idx);
    if (!cp)
      break;
    __memcpy((uint8_t *)buf + nread, cp->data + page_off, chunk);
    page_cache_release(cp);
    nread += chunk;
  }
  return (int)nread;
}

/* ==================== File write ==================== */

int fat32_write(struct inode *ip, uint64_t offset, const void *buf,
                size_t count) {
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
    if (chunk > count - written)
      chunk = count - written;

    /* Convert page_idx to cluster index for FAT chain walk */
    uint32_t clusters_per_page = 4096 / bytes_per_cluster;
    uint32_t cluster_idx = page_idx * clusters_per_page;
    /* The page cache is 4KB-granular: page_cache_fill reads and
     * page_cache_writeback writes every cluster spanning the page
     * (clusters_per_page of them — 8 when the FAT32 cluster is 512B). For the
     * writeback to persist the whole page, every cluster the write touches must
     * already exist in the chain; allocating only the first one (as the old
     * code did) leaves the rest unallocated, so writeback walks the chain, hits
     * EOF after the first cluster, and bails — silently dropping everything
     * past the first cluster and corrupting files >4KB. Allocate every cluster
     * covering [page_off, page_off+chunk) here, in order, tail-linked onto the
     * chain. */
    uint32_t first_in_page = page_off / bytes_per_cluster;
    uint32_t last_in_page = (page_off + chunk - 1) / bytes_per_cluster;
    bool enospc = false;
    for (uint32_t cip = first_in_page; cip <= last_in_page; cip++) {
      uint32_t existing =
          fat32_walk_chain(ip->start_cluster, cluster_idx + cip);
      if (existing >= 2 && existing < 0x0FFFFFF8)
        continue; /* already allocated */
      spin_lock(&fat_lock);
      uint32_t new_cluster = fat32_allocate_cluster();
      if (new_cluster == 0) {
        spin_unlock(&fat_lock);
        enospc = true;
        break; /* ENOSPC */
      }
      /* Zero-fill new cluster */
      uint8_t zero_buf[4096];
      __memset(zero_buf, 0, bytes_per_cluster);
      uint32_t lba = data_start_lba + (new_cluster - 2) * sectors_per_cluster;
      blk_write(lba, sectors_per_cluster, zero_buf);

      /* Link into chain. Detect "first cluster" by start_cluster == 0 alone:
       * ip->size is only updated AFTER the write loop (below), so it stays 0
       * for every iteration of a multi-page write — gating on ip->size == 0
       * would reset start_cluster to each newly-allocated cluster and skip the
       * tail link, orphaning all but the last cluster and corrupting >4KB
       * files. */
      if (ip->start_cluster == 0) {
        ip->start_cluster = new_cluster;
      } else {
        uint32_t tail = ip->start_cluster;
        int max_tail_walk = 1024;
        while (max_tail_walk-- > 0) {
          uint32_t next = fat32_read_entry(tail);
          if (next >= 0x0FFFFFF8)
            break;
          tail = next;
        }
        if (max_tail_walk <= 0) {
          WARN_ON(1);
          spin_unlock(&fat_lock);
          return -EIO;
        }
        fat32_link_cluster(tail, new_cluster);
      }
      spin_unlock(&fat_lock);

      /* Invalidate page cache — new cluster changes mapping */
      page_cache_invalidate_inode(ip);
    }
    if (enospc)
      break;

    struct cache_page *cp = page_cache_fill(ip, page_idx);
    if (!cp)
      break;
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

/* ==================== Mkdir ==================== */

int fat32_mkdir(const char *path) {
  /* Resolve parent directory */
  uint32_t parent_cluster, dummy_cluster;
  int dummy_idx;
  uint64_t dummy_size;
  int rc = fat32_resolve_path(path, &dummy_cluster, &parent_cluster, &dummy_idx,
                              &dummy_size, 1);
  if (rc != 0)
    return rc;

  /* Extract leaf name */
  int path_len = 0;
  while (path[path_len])
    path_len++;
  int last_slash = -1;
  for (int i = path_len - 1; i >= 0; i--) {
    if (path[i] == '/') {
      last_slash = i;
      break;
    }
  }
  const char *leaf = path + last_slash + 1;
  int leaf_len = path_len - last_slash - 1;

  if (leaf_len == 0)
    return -ENOENT;

  /* Allocate a cluster for the new directory */
  uint32_t new_cluster = fat32_allocate_cluster();
  if (new_cluster == 0)
    return -ENOSPC;

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
  for (int i = 1; i < 11; i++)
    dot->name[i] = ' ';
  dot->attr = 0x10;
  dot->fst_clus_hi = (new_cluster >> 16) & 0xFFFF;
  dot->fst_clus_lo = new_cluster & 0xFFFF;

  struct fat_dir_entry *dotdot = (struct fat_dir_entry *)(dir_buf + 32);
  __memset(dotdot, 0, 32);
  dotdot->name[0] = '.';
  dotdot->name[1] = '.';
  for (int i = 2; i < 11; i++)
    dotdot->name[i] = ' ';
  dotdot->attr = 0x10;
  dotdot->fst_clus_hi = (parent_cluster >> 16) & 0xFFFF;
  dotdot->fst_clus_lo = parent_cluster & 0xFFFF;

  /* Write new directory cluster to disk */
  uint32_t lba = data_start_lba + (new_cluster - 2) * sectors_per_cluster;
  blk_write(lba, sectors_per_cluster, dir_buf);
  kfree(dir_buf);

  /* Add entry in parent directory */
  uint8_t *parent_buf = read_cluster_buf(dummy_cluster);
  if (!parent_buf)
    return -EIO;

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
  int wrc = write_cluster_sector(dummy_cluster, sector_idx,
                                 parent_buf + sector_idx * 512);
  kfree(parent_buf);
  return wrc == 0 ? 0 : -EIO;
}

/* ==================== Unlink ==================== */

int fat32_unlink(const char *path) {
  uint32_t cluster, dir_cluster;
  int dir_idx;
  uint64_t file_size;

  int rc =
      fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx, &file_size, 0);
  if (rc != 0)
    return rc;

  /* Read directory entry to check it's not a directory */
  uint8_t *dir_buf = read_cluster_buf(dir_cluster);
  if (!dir_buf)
    return -EIO;
  struct fat_dir_entry *de = (struct fat_dir_entry *)(dir_buf + dir_idx * 32);

  if (de->attr & 0x10) {
    kfree(dir_buf);
    return -EISDIR;
  }

  uint32_t target_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;

  /* Invalidate page cache for this inode (lookup by dir-entry location ino). */
  struct inode *ip = inode_lookup(fat32_make_ino(dir_cluster, dir_idx));
  if (ip) {
    page_cache_invalidate_inode(ip);
    inode_put(ip);
  }

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

  return 0;
}

/* ==================== Rmdir ==================== */

int fat32_rmdir(const char *path) {
  uint32_t cluster, dir_cluster;
  int dir_idx;
  uint64_t file_size;

  int rc =
      fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx, &file_size, 0);
  if (rc != 0)
    return rc;

  /* Check it's a directory */
  uint8_t *dir_buf = read_cluster_buf(dir_cluster);
  if (!dir_buf)
    return -EIO;
  struct fat_dir_entry *de = (struct fat_dir_entry *)(dir_buf + dir_idx * 32);

  if (!(de->attr & 0x10)) {
    kfree(dir_buf);
    return -ENOTDIR;
  }

  uint32_t target_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
  if (target_cluster == 0)
    target_cluster = root_cluster;

  /* Check directory is empty (only . and ..) */
  uint32_t cc = target_cluster;
  int is_empty = 1;
  while (cc >= 2 && cc < 0x0FFFFFF8 && is_empty) {
    uint8_t *dbuf = read_cluster_buf(cc);
    if (!dbuf) {
      is_empty = 0;
      break;
    }
    int entries = bytes_per_cluster / 32;
    for (int i = 0; i < entries; i++) {
      struct fat_dir_entry *d = (struct fat_dir_entry *)(dbuf + i * 32);
      if (d->name[0] == 0x00)
        break;
      if (d->name[0] == 0xE5)
        continue;
      if (d->name[0] == '.' && (d->name[1] == ' ' || d->name[1] == '.'))
        continue;
      if (d->name[0] == '.' && d->name[1] == '.')
        continue;
      is_empty = 0;
      break;
    }
    kfree(dbuf);
    uint32_t next = fat32_read_entry(cc);
    if (next >= 0x0FFFFFF8)
      break;
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

  int rc =
      fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx, &file_size, 0);
  if (rc != 0)
    return rc;

  struct kstat *st = (void *)stat_buf;
  __memset(st, 0, sizeof(*st));
  st->st_ino = fat32_make_ino(dir_cluster, dir_idx);

  if (dir_idx < 0) {
    /* Reached via root, "." or ".." — no parent dir entry; treat as directory
     */
    st->st_mode = 0040755;
    st->st_nlink = 1;
    st->st_size = 0;
    st->st_blksize = bytes_per_cluster;
    return 0;
  }

  /* Read directory entry for attributes */
  uint8_t *dir_buf = read_cluster_buf(dir_cluster);
  if (!dir_buf)
    return -EIO;
  struct fat_dir_entry *de = (struct fat_dir_entry *)(dir_buf + dir_idx * 32);

  st->st_mode = (de->attr & 0x10) ? 0040755 : 0100644;
  st->st_nlink = 1;
  st->st_size = file_size;
  st->st_blksize = bytes_per_cluster;

  kfree(dir_buf);
  return 0;
}

/* ==================== getdents: read directory entries into user buffer
 * ==================== */
/* Each entry: struct dirent64 — defined in xos/dirent.h
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
    if (!cbuf)
      return -EIO;

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
      /* Skip LFN entries and volume labels */
      if (de->attr == 0x0F || de->attr & 0x08) {
        if (de->attr == 0x0F)
          collect_lfn_entry(de, lfn_buf);
        lfn_buf[0] = '\0';
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
        for (j = 0; lfn_buf[j] && j < 255; j++)
          name[j] = lfn_buf[j];
        name[j] = '\0';
      } else {
        /* Convert 8.3 name */
        int j = 0;
        for (int k = 0; k < 8 && de->name[k] != ' '; k++) {
          char c = de->name[k];
          if (c >= 'A' && c <= 'Z')
            c += 32;
          name[j++] = c;
        }
        if (de->name[8] != ' ') {
          name[j++] = '.';
          for (int k = 8; k < 11 && de->name[k] != ' '; k++) {
            char c = de->name[k];
            if (c >= 'A' && c <= 'Z')
              c += 32;
            name[j++] = c;
          }
        }
        name[j] = '\0';
      }
      lfn_buf[0] = '\0';

      /* Compute entry size: dirent64 header + name + null + padding to 8-byte
       * align */
      int name_len = 0;
      while (name[name_len])
        name_len++;
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
      d->d_ino = fat32_make_ino(scan_cluster, i);
      d->d_off = entry_idx;
      d->d_reclen = reclen;
      d->d_type = (de->attr & 0x10) ? DT_DIR : DT_REG;
      int j;
      for (j = 0; j < name_len; j++)
        d->d_name[j] = name[j];
      d->d_name[j] = '\0';

      written += reclen;
      entry_idx++;
    }

    kfree(cbuf);

    /* Follow FAT chain */
    uint32_t next = fat32_read_entry(scan_cluster);
    if (next >= 0x0FFFFFF8) {
      *pos = (uint64_t)-1; /* EOF */
      return (int)written;
    }
    scan_cluster = next;
  }

  *pos = (uint64_t)-1;
  return (int)written;
}

/* ==================== FAT32 fstype shims ==================== */
/* fat32_resolve_path requires path[0]=='/', so relpath callbacks prepend '/'.
 * fat32_getdents takes (dir_cluster, &pos, buf, len); shim extracts
 * dir->start_cluster and passes &ctx->pos. */

#include "kernel/bsd/mount.h"

static ssize_t fat32_fs_getdents(struct inode *dir, struct dir_context *ctx) {
  int ret = fat32_getdents(dir->start_cluster, &ctx->pos, ctx->buf, ctx->len);
  if (ret < 0)
    return (ssize_t)ret;
  ctx->written = (size_t)ret;
  return (ssize_t)ret;
}

struct fstype fat32_fstype = {
    .name = "fat32",
    .mount_root = fat32_mount_root,
    .getdents = fat32_fs_getdents,
};
