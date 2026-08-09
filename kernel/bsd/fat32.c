/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/fat32.c — in-kernel FAT32 filesystem (synchronous).
// Ported from driver/fs_driver.cc, converting async state machines to simple
// synchronous loops using blk_read/blk_write.
//
// Kernel constraint: C only (commit 18c91ca).
#include "kernel/bsd/fat32.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/x64/apic.h" // sched_clock() — realtime ns for inode timestamps
#include "arch/x64/rtc.h"  // wall_clock_boot_ns — RTC-epoch baseline
#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/inotify.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/bsd/types.h"
#include "kernel/driver/ahci.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/spinlock.h"

#include "kernel/bsd/kfcntl.h"
#include <kernel/bsd/stat_abi.h>
#include <xos/dirent.h>
#include <xos/errno.h>
#include <xos/page.h>

struct xtask;

// ==================== FAT32 volume state ====================
static uint32_t part_start_lba;
static uint32_t fat_start_lba;
static uint32_t data_start_lba;
static uint32_t root_cluster;
static uint32_t sectors_per_cluster;
static uint32_t bytes_per_cluster;
static uint32_t total_data_clusters;
static struct block_partition *fat32_part;
static struct super_block fat32_sb;

static int fat32_sync_fs(struct super_block *sb, bool wait);

#define FAT_I(ip) ((struct fat32_inode_info *)(ip)->i_private)

static void fat32_evict_inode(struct inode *ip) {
  if (ip->i_private) {
    kfree(ip->i_private);
    ip->i_private = NULL;
  }
}

static const struct super_operations fat32_sops = {
    .sync_fs = fat32_sync_fs,
    .evict_inode = fat32_evict_inode,
};

static void fat32_invalidate_pages(struct inode *ip) {
  if (FAT_I(ip))
    __atomic_store_n(&FAT_I(ip)->walk_cursor, 0, __ATOMIC_RELEASE);
  page_cache_invalidate_inode(ip);
}
static uint32_t spf32;
static uint32_t next_free_hint = 2;
static uint32_t fsinfo_sector;
static uint32_t fsinfo_backup_sector;
static uint32_t fsinfo_free_clusters = UINT32_MAX;
static bool fsinfo_valid;
static bool fsinfo_dirty;

// Global FAT lock: protects FAT modifications (free cluster scan + FAT entry
// writes). Long-term metadata serialization is independent of the AHCI queue.
static mutex fat_lock;
static mutex fat_cache_fill_lock;

#define FAT32_FSINFO_LEAD_SIG 0x41615252U
#define FAT32_FSINFO_STRUCT_SIG 0x61417272U
#define FAT32_FSINFO_TRAIL_SIG 0xAA550000U

static uint32_t fat32_load_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void fat32_store_u32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static bool fat32_fsinfo_signatures_valid(const uint8_t sector[512]) {
  return fat32_load_u32(sector) == FAT32_FSINFO_LEAD_SIG &&
         fat32_load_u32(sector + 484) == FAT32_FSINFO_STRUCT_SIG &&
         fat32_load_u32(sector + 508) == FAT32_FSINFO_TRAIL_SIG;
}

static void fat32_account_fat_transition(uint32_t old_value,
                                         uint32_t new_value) {
  if (!fsinfo_valid || fsinfo_free_clusters == UINT32_MAX)
    return;
  bool was_free = (old_value & 0x0FFFFFFF) == 0;
  bool is_free = (new_value & 0x0FFFFFFF) == 0;
  if (was_free == is_free)
    return;
  if (is_free) {
    if (fsinfo_free_clusters < total_data_clusters)
      fsinfo_free_clusters++;
  } else if (fsinfo_free_clusters > 0) {
    fsinfo_free_clusters--;
  }
  fsinfo_dirty = true;
}

// Caller holds fat_lock, which serializes the cached count with FAT updates.
static int fat32_sync_fsinfo_locked(void) {
  if (!fsinfo_valid || !fsinfo_dirty)
    return 0;
  uint8_t sector[512];
  if (partition_read(fat32_part, fsinfo_sector, 1, sector) != 0)
    return -EIO;
  if (!fat32_fsinfo_signatures_valid(sector)) {
    printk(LOG_WARN, "fat32: FSInfo signatures changed; refusing update\n");
    fsinfo_valid = false;
    return -EIO;
  }
  fat32_store_u32(sector + 488, fsinfo_free_clusters);
  fat32_store_u32(sector + 492, next_free_hint);
  if (partition_write(fat32_part, fsinfo_sector, 1, sector) != 0)
    return -EIO;
  if (fsinfo_backup_sector != UINT32_MAX &&
      partition_write(fat32_part, fsinfo_backup_sector, 1, sector) != 0)
    return -EIO;
  fsinfo_dirty = false;
  return 0;
}

static int fat32_sync_fs(struct super_block *sb, bool wait) {
  (void)sb;
  (void)wait;
  mutex_lock(&fat_lock);
  int rc = fat32_sync_fsinfo_locked();
  mutex_unlock(&fat_lock);
  return rc;
}

// ==================== FAT sector cache ====================
// Keep the common root-volume FAT resident. The shipped image has a 3.7 MiB
// FAT, while a 64 KiB cache thrashed during random faults of the LLVM DSOs and
// reread hundreds of MiB of FAT metadata. Larger volumes still use this as a
// bounded direct-mapped cache.
#define FAT_CACHE_PAGES 8192
#define FAT_CACHE_READAHEAD_SECTORS AHCI_MAX_SECTORS

#if (FAT_CACHE_PAGES & (FAT_CACHE_PAGES - 1)) != 0
#error "FAT_CACHE_PAGES must be a power of two"
#endif
#if (FAT_CACHE_PAGES % FAT_CACHE_READAHEAD_SECTORS) != 0
#error "FAT cache groups must not wrap the data array"
#endif

struct fat_cache_entry {
  uint32_t sector_lba;
  uint32_t generation;
  bool filling;
};

static struct fat_cache_entry fat_cache[FAT_CACHE_PAGES];
static uint8_t fat_cache_data[FAT_CACHE_PAGES][512]
    __attribute__((aligned(4096)));
static spinlock fat_cache_lock = SPINLOCK_INIT;

static uint64_t fat_cache_hits;
static uint64_t fat_cache_misses;
static uint64_t fat_cache_fill_waits;
static uint64_t fat_cache_io_commands;
static uint64_t fat_cache_io_sectors;
static uint64_t fat_batch_count;
static uint64_t fat_sector_reads;
static uint64_t fat1_writes;
static uint64_t fat2_writes;
static uint64_t fat_zero_commands;
static struct fat32_stats fat_stats;
static uint8_t fat_zero_region[AHCI_MAX_SECTORS * 512]
    __attribute__((aligned(4096)));

static int fat_cache_slot(uint32_t sector_lba) {
  return (int)((sector_lba - fat_start_lba) & (FAT_CACHE_PAGES - 1));
}

// Read FAT sector into cache, returns cache slot.
//
// SMP-safe fill: reserve and invalidate the victim before I/O, then publish it
// under fat_cache_lock only after the sector is complete. Cache misses are
// serialized by a sleepable mutex so a task never spins behind a filler that
// blocked in the AHCI path.
static int fat_cache_read(uint32_t sector_lba) {
  uint32_t relative = sector_lba - fat_start_lba;
  uint32_t group_relative =
      relative & ~(uint32_t)(FAT_CACHE_READAHEAD_SECTORS - 1);
  uint32_t group_lba = fat_start_lba + group_relative;
  uint32_t group_count = FAT_CACHE_READAHEAD_SECTORS;
  if (group_relative + group_count > spf32)
    group_count = spf32 - group_relative;
  int slot = fat_cache_slot(sector_lba);
  int group_slot = (int)(group_relative & (FAT_CACHE_PAGES - 1));
  uint32_t generations[FAT_CACHE_READAHEAD_SECTORS];

  spin_lock(&fat_cache_lock);
  if (!fat_cache[slot].filling && fat_cache[slot].sector_lba == sector_lba) {
    __atomic_fetch_add(&fat_cache_hits, 1, __ATOMIC_RELAXED);
    spin_unlock(&fat_cache_lock);
    return slot;
  }
  spin_unlock(&fat_cache_lock);

  mutex_lock(&fat_cache_fill_lock);

  // Another filler may have populated this sector while we slept.
  spin_lock(&fat_cache_lock);
  if (!fat_cache[slot].filling && fat_cache[slot].sector_lba == sector_lba) {
    __atomic_fetch_add(&fat_cache_hits, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&fat_cache_fill_waits, 1, __ATOMIC_RELAXED);
    spin_unlock(&fat_cache_lock);
    mutex_unlock(&fat_cache_fill_lock);
    return slot;
  }
  for (uint32_t i = 0; i < group_count; i++) {
    struct fat_cache_entry *entry = &fat_cache[group_slot + i];
    entry->sector_lba = group_lba + i;
    entry->filling = true;
    generations[i] = ++entry->generation;
  }
  __atomic_fetch_add(&fat_cache_misses, 1, __ATOMIC_RELAXED);
  spin_unlock(&fat_cache_lock);

  int rc = partition_read(fat32_part, group_lba, group_count,
                          fat_cache_data[group_slot]);
  __atomic_fetch_add(&fat_cache_io_commands, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&fat_cache_io_sectors, group_count, __ATOMIC_RELAXED);

  spin_lock(&fat_cache_lock);
  for (uint32_t i = 0; i < group_count; i++) {
    struct fat_cache_entry *entry = &fat_cache[group_slot + i];
    if (entry->generation != generations[i] || rc != 0)
      entry->sector_lba = 0xFFFFFFFF;
    entry->filling = false;
  }
  spin_unlock(&fat_cache_lock);
  mutex_unlock(&fat_cache_fill_lock);
  return rc == 0 ? slot : -1;
}

// Invalidate FAT cache entries for a given sector.
static void fat_cache_invalidate_sector(uint32_t sector_lba) {
  spin_lock(&fat_cache_lock);
  int slot = fat_cache_slot(sector_lba);
  if (fat_cache[slot].sector_lba == sector_lba) {
    fat_cache[slot].sector_lba = 0xFFFFFFFF;
    fat_cache[slot].generation++;
  }
  spin_unlock(&fat_cache_lock);
}

// ==================== FAT entry read/write ====================

// Read a FAT entry (synchronous, uses FAT cache).
static uint32_t fat32_read_entry(uint32_t cluster) {
  uint32_t fat_offset = cluster * 4;
  uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
  uint32_t offset_in_sector = fat_offset % 512;

  for (;;) {
    int slot = fat_cache_read(fat_sector);
    if (slot < 0)
      return 0x0FFFFFFF;

    spin_lock(&fat_cache_lock);
    if (!fat_cache[slot].filling && fat_cache[slot].sector_lba == fat_sector) {
      const uint8_t *src = fat_cache_data[slot] + offset_in_sector;
      uint32_t entry_val = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
      spin_unlock(&fat_cache_lock);
      return entry_val & 0x0FFFFFFF;
    }
    spin_unlock(&fat_cache_lock);
    // The slot was evicted after fat_cache_read() returned; retry lookup.
  }
}

// Write a FAT entry (dual-write to FAT1 and FAT2).
static int fat32_write_fat_entry(uint32_t cluster, uint32_t value) {
  uint32_t fat_offset = cluster * 4;
  uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
  uint32_t offset_in_sector = fat_offset % 512;

  uint8_t sector_buf[512];
  if (partition_read(fat32_part, fat_sector, 1, sector_buf) != 0)
    return -EIO;

  uint8_t *p = sector_buf + offset_in_sector;
  uint32_t old = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  uint32_t nv = (old & 0xF0000000) | (value & 0x0FFFFFFF);
  p[0] = nv & 0xFF;
  p[1] = (nv >> 8) & 0xFF;
  p[2] = (nv >> 16) & 0xFF;
  p[3] = (nv >> 24) & 0xFF;

  // Write FAT1.
  if (partition_write(fat32_part, fat_sector, 1, sector_buf) != 0)
    return -EIO;
  fat32_account_fat_transition(old, nv);
  // Write FAT2.
  if (partition_write(fat32_part, fat_sector + spf32, 1, sector_buf) != 0)
    return -EIO;

  // Invalidate cache for this sector so subsequent reads get fresh data.
  fat_cache_invalidate_sector(fat_sector);
  fat_cache_invalidate_sector(fat_sector + spf32);

  return 0;
}

// ==================== FAT chain walk ====================

// Walk FAT chain from start_cluster, return the cluster at page_index.
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

// Walk the FAT chain to the cluster at cluster_index, resuming from the inode's
// forward-only cursor when the target is at or beyond it. Sequential reads then
// touch each FAT entry once total (O(n)) instead of restarting at the chain
// head every page (O(n²)). Cursor packs (index<<32)|cluster; an out-of-range
// cursor (holding an EOF marker or positioned after the target) falls back to a
// head start, and chain mutations invalidate it through the page cache. The
// packed pair is loaded and stored atomically so concurrent faults cannot tear
// it. Letting the most recently completed walk publish its position also keeps
// the cursor near the active mmap fault region instead of pinning it at the
// highest index seen.
uint32_t fat32_walk_chain_cached(struct inode *ip, uint64_t cluster_index,
                                 enum fat32_walk_source source) {
  if (source != FAT32_WALK_DEMAND && source != FAT32_WALK_READAHEAD) {
    source = FAT32_WALK_DEMAND;
    __atomic_fetch_add(&fat_stats.walk_invalid[source], 1, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&fat_stats.walk_calls[source], 1, __ATOMIC_RELAXED);
  uint64_t cur = __atomic_load_n(&FAT_I(ip)->walk_cursor, __ATOMIC_ACQUIRE);
  uint32_t cur_idx = (uint32_t)(cur >> 32);
  uint32_t c = (uint32_t)(cur & 0xFFFFFFFF);
  if (c < 2 || c >= 0x0FFFFFF8 || cur_idx > cluster_index) {
    // Cursor unusable, holds an EOF marker, or target is behind it — restart at
    // the chain head.
    __atomic_fetch_add(&fat_stats.walk_head_restarts[source], 1,
                       __ATOMIC_RELAXED);
    if (cur_idx > cluster_index)
      __atomic_fetch_add(&fat_stats.walk_backtracks[source], 1,
                         __ATOMIC_RELAXED);
    cur_idx = 0;
    c = FAT_I(ip)->start_cluster;
  }
  while (cur_idx < cluster_index) {
    if (c < 2 || c >= 0x0FFFFFF8) {
      __atomic_fetch_add(&fat_stats.walk_invalid[source], 1, __ATOMIC_RELAXED);
      return c; // EOF before target; leave the cursor at the last valid cluster
    }
    uint32_t next = fat32_read_entry(c);
    __atomic_fetch_add(&fat_stats.walk_steps[source], 1, __ATOMIC_RELAXED);
    if (next < 2 || next >= 0x0FFFFFF8) {
      __atomic_fetch_add(&fat_stats.walk_invalid[source], 1, __ATOMIC_RELAXED);
      return next; // EOF at the target boundary
    }
    c = next;
    cur_idx++;
  }
  // Reached cluster_index. Publish only a valid data cluster; EOF markers
  // cannot be resumed from.
  if (c >= 2 && c < 0x0FFFFFF8)
    __atomic_store_n(&FAT_I(ip)->walk_cursor,
                     ((uint64_t)cur_idx << 32) | (uint64_t)c, __ATOMIC_RELEASE);
  return c;
}

void fat32_get_stats(struct fat32_stats *out) {
  if (!out)
    return;
  out->cache_hits = __atomic_load_n(&fat_cache_hits, __ATOMIC_RELAXED);
  out->cache_misses = __atomic_load_n(&fat_cache_misses, __ATOMIC_RELAXED);
  out->cache_fill_waits =
      __atomic_load_n(&fat_cache_fill_waits, __ATOMIC_RELAXED);
  out->cache_io_commands =
      __atomic_load_n(&fat_cache_io_commands, __ATOMIC_RELAXED);
  out->cache_io_sectors =
      __atomic_load_n(&fat_cache_io_sectors, __ATOMIC_RELAXED);
  for (unsigned i = 0; i < 2; i++) {
    out->walk_calls[i] =
        __atomic_load_n(&fat_stats.walk_calls[i], __ATOMIC_RELAXED);
    out->walk_steps[i] =
        __atomic_load_n(&fat_stats.walk_steps[i], __ATOMIC_RELAXED);
    out->walk_head_restarts[i] =
        __atomic_load_n(&fat_stats.walk_head_restarts[i], __ATOMIC_RELAXED);
    out->walk_backtracks[i] =
        __atomic_load_n(&fat_stats.walk_backtracks[i], __ATOMIC_RELAXED);
    out->walk_invalid[i] =
        __atomic_load_n(&fat_stats.walk_invalid[i], __ATOMIC_RELAXED);
    out->mapped_sectors[i] =
        __atomic_load_n(&fat_stats.mapped_sectors[i], __ATOMIC_RELAXED);
  }
}

void fat32_account_mapped_sector(enum fat32_walk_source source) {
  fat32_account_mapped_sectors(source, 1);
}

void fat32_account_mapped_sectors(enum fat32_walk_source source,
                                  uint32_t count) {
  if (source != FAT32_WALK_DEMAND && source != FAT32_WALK_READAHEAD)
    source = FAT32_WALK_DEMAND;
  __atomic_fetch_add(&fat_stats.mapped_sectors[source], count,
                     __ATOMIC_RELAXED);
}

// ==================== Cluster allocation ====================

// Find a free cluster and mark it as EOF. Returns cluster number or 0 on
// failure. Caller holds fat_lock (so no concurrent FAT writer), but FAT readers
// (directory lookups) don't take fat_lock and can refill/evict a shared
// fat_cache slot mid-scan, which would let us read a buffer changing under us.
// Read the sector into a private stack buffer so the scan bytes are stable.
static int fat32_allocate_run(uint32_t wanted, uint32_t *first, uint32_t *last,
                              uint32_t *allocated);

static uint32_t fat32_allocate_cluster(void) {
  uint32_t first = 0;
  uint32_t last = 0;
  uint32_t allocated = 0;
  if (fat32_allocate_run(1, &first, &last, &allocated) != 0)
    return 0;
  return first;
}

static void fat32_store_entry(uint8_t *p, uint32_t value) {
  uint32_t old = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  uint32_t nv = (old & 0xF0000000) | (value & 0x0FFFFFFF);
  p[0] = (uint8_t)nv;
  p[1] = (uint8_t)(nv >> 8);
  p[2] = (uint8_t)(nv >> 16);
  p[3] = (uint8_t)(nv >> 24);
}

/* Allocate one physically contiguous run from a single FAT sector. */
static int fat32_allocate_run(uint32_t wanted, uint32_t *first, uint32_t *last,
                              uint32_t *allocated) {
  if (!wanted || !first || !last || !allocated)
    return -EINVAL;
  uint8_t sec[512];
  for (uint32_t sector = 0; sector < spf32; sector++) {
    uint32_t abs_sector = ((next_free_hint / 128) + sector) % spf32;
    if (partition_read(fat32_part, fat_start_lba + abs_sector, 1, sec) != 0)
      continue;
    __atomic_fetch_add(&fat_sector_reads, 1, __ATOMIC_RELAXED);

    for (int i = 0; i < 128; i++) {
      uint32_t c = abs_sector * 128 + i;
      if (c < 2 || c >= total_data_clusters + 2)
        continue;
      const uint8_t *p = sec + i * 4;
      uint32_t e = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      e &= 0x0FFFFFFF;
      if (e == 0) {
        uint32_t run = 1;
        while (run < wanted && i + (int)run < 128) {
          uint32_t candidate = c + run;
          if (candidate >= total_data_clusters + 2)
            break;
          const uint8_t *q = sec + (i + run) * 4;
          uint32_t qe = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                        ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
          if ((qe & 0x0FFFFFFF) != 0)
            break;
          run++;
        }
        for (uint32_t j = 0; j < run; j++)
          fat32_store_entry(sec + (i + j) * 4,
                            j + 1 < run ? c + j + 1 : 0x0FFFFFFF);

        uint32_t lba = fat_start_lba + abs_sector;
        if (partition_write(fat32_part, lba, 1, sec) != 0)
          return -EIO;
        for (uint32_t j = 0; j < run; j++)
          fat32_account_fat_transition(0, 0x0FFFFFFF);
        __atomic_fetch_add(&fat1_writes, 1, __ATOMIC_RELAXED);
        if (partition_write(fat32_part, lba + spf32, 1, sec) != 0)
          return -EIO;
        __atomic_fetch_add(&fat2_writes, 1, __ATOMIC_RELAXED);
        fat_cache_invalidate_sector(lba);
        fat_cache_invalidate_sector(lba + spf32);
        __atomic_fetch_add(&fat_batch_count, 1, __ATOMIC_RELAXED);

        *first = c;
        *last = c + run - 1;
        *allocated = run;
        next_free_hint = c + run;
        if (next_free_hint >= total_data_clusters + 2)
          next_free_hint = 2;
        fsinfo_dirty = fsinfo_valid;
        return 0;
      }
    }
  }
  return -ENOSPC;
}

static bool fat32_chain_step(uint32_t cluster, uint64_t cluster_limit,
                             uint32_t *next, bool *at_end) {
  if (cluster >= 0x0FFFFFF8) {
    *at_end = true;
    return true;
  }
  if (cluster < 2 || cluster >= cluster_limit) {
    printk(LOG_WARN, "fat32: invalid cluster %u in chain\n", cluster);
    return false;
  }
  *next = fat32_read_entry(cluster);
  *at_end = *next >= 0x0FFFFFF8;
  if (!*at_end && (*next < 2 || *next >= cluster_limit)) {
    printk(LOG_WARN, "fat32: cluster %u points outside volume to %u\n", cluster,
           *next);
    return false;
  }
  return true;
}

// Validate before mutation so clearing entries cannot hide a cycle that loops
// back through an already-freed cluster.
static bool fat32_chain_is_acyclic(uint32_t start_cluster,
                                   uint64_t cluster_limit) {
  uint32_t slow = start_cluster;
  uint32_t fast = start_cluster;
  uint64_t max_steps = cluster_limit - 2;
  for (uint64_t step = 0; step < max_steps; step++) {
    bool at_end;
    if (!fat32_chain_step(slow, cluster_limit, &slow, &at_end))
      return false;
    if (at_end)
      return true;
    if (!fat32_chain_step(fast, cluster_limit, &fast, &at_end))
      return false;
    if (at_end)
      return true;
    if (!fat32_chain_step(fast, cluster_limit, &fast, &at_end))
      return false;
    if (at_end)
      return true;
    if (slow == fast) {
      printk(LOG_WARN, "fat32: cycle detected at cluster %u\n", slow);
      return false;
    }
  }
  return false;
}

// Free an entire cluster chain starting from start_cluster.
static void fat32_free_chain(uint32_t start_cluster) {
  if (start_cluster < 2 || start_cluster >= 0x0FFFFFF8)
    return;

  uint64_t fat_entry_limit = (uint64_t)spf32 * (512 / sizeof(uint32_t));
  uint64_t cluster_limit =
      total_data_clusters ? (uint64_t)total_data_clusters + 2 : fat_entry_limit;
  if (cluster_limit > fat_entry_limit)
    cluster_limit = fat_entry_limit;
  if (cluster_limit <= 2 ||
      !fat32_chain_is_acyclic(start_cluster, cluster_limit)) {
    WARN_ON(1);
    return;
  }

  uint32_t c = start_cluster;
  uint64_t remaining = cluster_limit - 2;
  while (c >= 2 && c < 0x0FFFFFF8 && remaining-- > 0) {
    uint32_t next = fat32_read_entry(c);
    if (fat32_write_fat_entry(c, 0) != 0)
      break;
    if (c < next_free_hint) {
      next_free_hint = c;
      fsinfo_dirty = fsinfo_valid;
    }
    if (next >= 0x0FFFFFF8)
      break;
    c = next;
  }
}

// Link a new cluster after tail_cluster in the FAT chain.
static int fat32_link_cluster(uint32_t tail_cluster, uint32_t new_cluster) {
  return fat32_write_fat_entry(tail_cluster, new_cluster);
}

static int fat32_allocate_detached(uint32_t count, uint32_t *chain_start,
                                   uint32_t *chain_tail) {
  uint32_t start = 0;
  uint32_t tail = 0;
  uint32_t max_clusters = AHCI_MAX_SECTORS / sectors_per_cluster;
  if (max_clusters == 0)
    return -EIO;

  while (count > 0) {
    uint32_t wanted = count < max_clusters ? count : max_clusters;
    uint32_t run_start, run_tail, got;
    int rc = fat32_allocate_run(wanted, &run_start, &run_tail, &got);
    if (rc) {
      if (start)
        fat32_free_chain(start);
      return rc;
    }

    uint32_t lba = data_start_lba + (run_start - 2) * sectors_per_cluster;
    rc = partition_write(fat32_part, lba, got * sectors_per_cluster,
                         fat_zero_region);
    __atomic_fetch_add(&fat_zero_commands, 1, __ATOMIC_RELAXED);
    if (rc) {
      fat32_free_chain(run_start);
      if (start)
        fat32_free_chain(start);
      return rc;
    }

    if (!start)
      start = run_start;
    else if (fat32_link_cluster(tail, run_start) != 0) {
      fat32_free_chain(run_start);
      fat32_free_chain(start);
      return -EIO;
    }
    tail = run_tail;
    count -= got;
  }
  *chain_start = start;
  *chain_tail = tail;
  return 0;
}

// ==================== 8.3 name helpers ====================

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
  // A leading dot is only legal for the special entries above. Map names such
  // as ".cache" to a stable, legal short alias that lookup canonicalizes too.
  if (user[0] == '.') {
    out[j++] = '_';
    while (i < user_len && user[i] == '.')
      i++;
  }
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

// ==================== LFN helpers ====================

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

// ==================== Volume geometry accessors ====================

uint32_t fat32_data_start_lba(void) { return data_start_lba; }
struct block_partition *fat32_partition(void) { return fat32_part; }
uint32_t fat32_sectors_per_cluster(void) { return sectors_per_cluster; }
uint32_t fat32_bytes_per_cluster(void) { return bytes_per_cluster; }

// ==================== Inode numbering ====================
//
// FAT32 has no on-disk inode number. We previously used the file's
// start_cluster as the ino, but a freshly created empty file has start_cluster
// == 0, so every empty file mapped to ino 0 and shared a single inode cache
// entry (data corruption), and it also collided with devtmpfs directory inodes
// that used ino 0. Use the stable, unique location of the file's directory
// entry instead: the entry's directory cluster and its index within that
// cluster. This is unique even for empty files (their dir entry exists before
// any cluster is allocated).
//
// Layout: ino = dir_cluster * entries_per_cluster + dir_entry_index,
// which stays below the devtmpfs range (0x80000000+) for any realistic disk.
// The root directory (dir_idx < 0) keeps ino = root_cluster.
static uint32_t fat32_make_ino(uint32_t dir_cluster, int dir_entry_idx) {
  if (dir_entry_idx < 0)
    return root_cluster;
  return dir_cluster * (bytes_per_cluster / 32) + (uint32_t)dir_entry_idx;
}

// Forward declarations: i_op tables are defined in R2-10; cluster read/write
// helpers are defined later in this file.
static const struct inode_operations fat32_dir_iop;
static const struct inode_operations fat32_file_iop;
static const struct address_space_operations fat32_aops;
static int fat32_dir_rename(struct inode *old_parent, const char *old_name,
                            struct inode *new_parent, const char *new_name);
static uint8_t *read_cluster_buf(uint32_t cluster);
static int write_cluster_sector(uint32_t cluster, int sector_idx,
                                const uint8_t *data);

// fat32_iget: via inode_get_or_create, get/create an inode and set i_op (only
// exit point). Cache hit reuses the same ino (idempotent i_op assignment — same
// value, no race); miss creates new. fat32 inodes have no fs-internal strong
// ref and can be reaped, hence inode_get_or_create.
static struct inode *fat32_iget(uint32_t ino, int type, uint64_t size,
                                uint32_t cluster, uint32_t dir_cluster,
                                int dir_idx) {
  struct inode *ip = inode_get_or_create(&fat32_sb, ino, type, size);
  if (!ip)
    return NULL;
  if (!ip->i_private) {
    struct fat32_inode_info *info = kmalloc(sizeof(*info));
    if (!info) {
      inode_put(ip);
      return NULL;
    }
    info->start_cluster = cluster;
    info->dir_start_cluster = dir_cluster;
    info->dir_entry_index = dir_idx;
    info->walk_cursor = 0;
    mutex_lock(&ip->i_lock);
    if (!ip->i_private)
      ip->i_private = info;
    else
      kfree(info);
    mutex_unlock(&ip->i_lock);
  }
  ip->i_op = (type == INODE_DIR) ? &fat32_dir_iop : &fat32_file_iop;
  ip->i_fop = type == INODE_REGULAR ? &fat32_file_fops : NULL;
  ip->i_aop = (type == INODE_REGULAR || type == INODE_DIR) ? &fat32_aops : NULL;
  // Q5: fat32 inodes have no fs-internal strong ref; after utimensat releases
  // them they can be reaped, and a later lookup builds a new inode whose
  // timestamps reset to 0. On cache miss (new build, refcount==1) initialize
  // atime/mtime/ctime to the current realtime ns so stat always returns a
  // non-zero mtime (never 0); explicit utimensat values are lost once the inode
  // is reaped (not persisted, acceptable). Cache hit reuses the old inode and
  // keeps the utimensat-written timestamps.
  if (refcount_read(&ip->i_count) == 1) {
    uint64_t now =
        __atomic_load_n(&wall_clock_boot_ns, __ATOMIC_RELAXED) + sched_clock();
    struct vfs_timespec64 ts = {.tv_sec = (int64_t)(now / 1000000000ULL),
                                .tv_nsec = (uint32_t)(now % 1000000000ULL)};
    ip->atime = ip->mtime = ip->ctime = ts;
  }
  return ip;
}

static ssize_t fat32_fop_read_at(struct xtask *proc, struct file *file,
                                 void *buf, size_t count, uint64_t offset) {
  (void)proc;
  return fat32_read(file->inode, offset, buf, count);
}

static ssize_t fat32_fop_read(struct xtask *proc, struct file *file, void *buf,
                              size_t count) {
  ssize_t rc = fat32_fop_read_at(proc, file, buf, count, file->offset);
  if (rc > 0)
    file->offset += (uint64_t)rc;
  return rc;
}

static ssize_t fat32_fop_write(struct xtask *proc, struct file *file,
                               const void *buf, size_t count) {
  (void)proc;
  uint64_t offset = (file->flags & O_APPEND) ? file->inode->size : file->offset;
  int rc = fat32_write(file->inode, offset, buf, count);
  if (rc > 0)
    file->offset = offset + (uint64_t)rc;
  return rc;
}

static int fat32_fop_fsync(struct file *file, bool datasync) {
  (void)file;
  (void)datasync;
  return fat32_sync_fs(&fat32_sb, true);
}

const struct file_operations fat32_file_fops = {
    .read = fat32_fop_read,
    .read_at = fat32_fop_read_at,
    .write = fat32_fop_write,
    .fsync = fat32_fop_fsync,
};

static int fat32_map_file_sector(struct inode *ip, uint64_t file_sector,
                                 uint64_t *sector) {
  if (!sectors_per_cluster)
    return -EIO;
  uint64_t cluster_index = file_sector / sectors_per_cluster;
  if (cluster_index > UINT32_MAX)
    return -EFBIG;
  uint32_t cluster =
      fat32_walk_chain_cached(ip, (uint32_t)cluster_index, FAT32_WALK_DEMAND);
  if (cluster < 2 || cluster >= 0x0ffffff8)
    return -EIO;
  uint64_t relative = (uint64_t)data_start_lba +
                      (uint64_t)(cluster - 2) * sectors_per_cluster +
                      file_sector % sectors_per_cluster;
  if (relative >= fat32_part->sector_count)
    return -EIO;
  *sector = relative;
  fat32_account_mapped_sector(FAT32_WALK_DEMAND);
  return 0;
}

static int fat32_readpage(struct inode *ip, uint64_t page_index, void *page) {
  if (!ip || !page)
    return -EINVAL;
  __memset(page, 0, PAGE_SIZE);
  if (page_index > UINT64_MAX / PAGE_SIZE)
    return -EFBIG;
  uint64_t byte_offset = page_index * PAGE_SIZE;
  if (byte_offset >= ip->size)
    return 0;
  uint64_t available = ip->size - byte_offset;
  if (available > PAGE_SIZE)
    available = PAGE_SIZE;
  uint32_t sectors = (uint32_t)((available + 511) / 512);
  uint64_t disk_sectors[PAGE_SIZE / 512];
  for (uint32_t i = 0; i < sectors; i++) {
    int rc = fat32_map_file_sector(ip, byte_offset / 512 + i, &disk_sectors[i]);
    if (rc)
      return rc;
  }

  for (uint32_t i = 0; i < sectors;) {
    uint32_t run = 1;
    while (i + run < sectors && disk_sectors[i + run] == disk_sectors[i] + run)
      run++;
    int rc = partition_read(fat32_part, disk_sectors[i], run,
                            (uint8_t *)page + (size_t)i * 512);
    if (rc)
      return rc;
    i += run;
  }
  return 0;
}

static int fat32_readpages(struct inode *ip, uint64_t first_page, void **pages,
                           size_t nr_pages,
                           struct address_space_read_stats *stats) {
  if (!ip || !pages || !nr_pages || nr_pages > PAGE_CACHE_RA_MAX_PAGES ||
      first_page > UINT64_MAX / PAGE_SIZE)
    return -EINVAL;
  for (size_t i = 0; i < nr_pages; i++)
    if (!pages[i])
      return -EINVAL;

  size_t staging_size = nr_pages * PAGE_SIZE;
  uint8_t *staging = kmalloc(staging_size);
  if (!staging)
    return -ENOMEM;
  __memset(staging, 0, staging_size);
  if (stats)
    __memset(stats, 0, sizeof(*stats));

  uint64_t byte_offset = first_page * PAGE_SIZE;
  uint64_t available = byte_offset < ip->size ? ip->size - byte_offset : 0;
  if (available > staging_size)
    available = staging_size;
  uint32_t total_sectors = (uint32_t)((available + 511) / 512);
  uint32_t logical_sector = 0;
  uint32_t run_logical = 0;
  uint32_t run_count = 0;
  uint64_t run_disk = 0;
  int rc = 0;

  while (logical_sector < total_sectors) {
    uint64_t file_sector = byte_offset / 512 + logical_sector;
    if (!sectors_per_cluster) {
      rc = -EIO;
      break;
    }
    uint64_t cluster_index = file_sector / sectors_per_cluster;
    if (cluster_index > UINT32_MAX) {
      rc = -EFBIG;
      break;
    }
    enum fat32_walk_source source = logical_sector < PAGE_SIZE / 512
                                        ? FAT32_WALK_DEMAND
                                        : FAT32_WALK_READAHEAD;
    uint32_t cluster =
        fat32_walk_chain_cached(ip, (uint32_t)cluster_index, source);
    if (cluster < 2 || cluster >= 0x0FFFFFF8) {
      rc = -EIO;
      break;
    }
    uint32_t in_cluster = (uint32_t)(file_sector % sectors_per_cluster);
    uint32_t extent = sectors_per_cluster - in_cluster;
    if (extent > total_sectors - logical_sector)
      extent = total_sectors - logical_sector;
    uint64_t disk_sector = (uint64_t)data_start_lba +
                           (uint64_t)(cluster - 2) * sectors_per_cluster +
                           in_cluster;
    if (disk_sector >= fat32_part->sector_count ||
        extent > fat32_part->sector_count - disk_sector) {
      rc = -EIO;
      break;
    }

    if (run_count && disk_sector != run_disk + run_count) {
      rc = partition_read(fat32_part, run_disk, run_count,
                          staging + (size_t)run_logical * 512);
      if (stats) {
        stats->io_commands++;
        stats->io_sectors += run_count;
        stats->fragment_splits++;
      }
      if (rc)
        break;
      run_count = 0;
    }
    if (!run_count) {
      run_disk = disk_sector;
      run_logical = logical_sector;
    }
    run_count += extent;
    uint32_t demand_sectors = 0;
    if (logical_sector < PAGE_SIZE / 512) {
      demand_sectors = PAGE_SIZE / 512 - logical_sector;
      if (demand_sectors > extent)
        demand_sectors = extent;
      fat32_account_mapped_sectors(FAT32_WALK_DEMAND, demand_sectors);
    }
    if (extent > demand_sectors)
      fat32_account_mapped_sectors(FAT32_WALK_READAHEAD,
                                   extent - demand_sectors);
    logical_sector += extent;
  }
  if (!rc && run_count) {
    rc = partition_read(fat32_part, run_disk, run_count,
                        staging + (size_t)run_logical * 512);
    if (stats) {
      stats->io_commands++;
      stats->io_sectors += run_count;
    }
  }
  if (!rc)
    for (size_t i = 0; i < nr_pages; i++)
      __memcpy(pages[i], staging + i * PAGE_SIZE, PAGE_SIZE);
  kfree(staging);
  return rc;
}

static int fat32_writepages(struct inode *ip, struct cache_page **pages,
                            size_t nr_pages) {
  if (!ip || !pages || !nr_pages)
    return -EINVAL;
  for (size_t p = 0; p < nr_pages; p++) {
    struct cache_page *cp = pages[p];
    if (!cp || cp->inode != ip || !cp->data ||
        cp->page_index > UINT64_MAX / PAGE_SIZE)
      return -EINVAL;
    uint64_t byte_offset = cp->page_index * PAGE_SIZE;
    if (byte_offset >= ip->size)
      continue;
    uint64_t available = ip->size - byte_offset;
    if (available > PAGE_SIZE)
      available = PAGE_SIZE;
    uint32_t sectors = (uint32_t)((available + 511) / 512);
    uint32_t run_first = 0;
    uint32_t run_count = 0;
    uint64_t run_sector = 0;
    for (uint32_t i = 0; i < sectors; i++) {
      uint64_t disk_sector;
      int rc = fat32_map_file_sector(ip, byte_offset / 512 + i, &disk_sector);
      if (rc)
        return rc;
      if (run_count && disk_sector != run_sector + run_count) {
        rc = partition_write(fat32_part, run_sector, run_count,
                             cp->data + (size_t)run_first * 512);
        if (rc)
          return rc;
        run_count = 0;
      }
      if (!run_count) {
        run_sector = disk_sector;
        run_first = i;
      }
      run_count++;
    }
    if (run_count) {
      int rc = partition_write(fat32_part, run_sector, run_count,
                               cp->data + (size_t)run_first * 512);
      if (rc)
        return rc;
    }
  }
  return 0;
}

static const struct address_space_operations fat32_aops = {
    .readpage = fat32_readpage,
    .readpages = fat32_readpages,
    .writepages = fat32_writepages,
};

// fat32_lookup_in_dir: scan the whole FAT chain of dir_cluster for an entry
// named `name`; returns 0 on hit (fills out_*) / -ENOENT if not found / -EIO.
// Extracted from fat32_resolve_path's inline scan loop (§6.5 sole directory
// scan primitive).
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
    if (partition_read(fat32_part, lba, sectors_per_cluster, buf) != 0) {
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

// fat32_dir_lookup: find a direct child named `name` in dir; returns +1 inode
// or NULL.
static struct inode *fat32_dir_lookup(struct inode *dir, const char *name) {
  uint32_t cluster, dir_cluster;
  int dir_idx, is_dir;
  uint64_t size;
  int rc = fat32_lookup_in_dir(FAT_I(dir)->start_cluster, name, &cluster,
                               &dir_cluster, &dir_idx, &size, &is_dir);
  if (rc != 0)
    return NULL;
  int type = is_dir ? INODE_DIR : INODE_REGULAR;
  uint32_t ino = fat32_make_ino(dir_cluster, dir_idx);
  return fat32_iget(ino, type, size, cluster, dir_cluster, dir_idx);
}

// fat32_dir_find_slot: find a free dir-entry slot (0x00 or 0xE5) on the cluster
// chain start_cluster; if no free slot on the chain, extend it with a zeroed
// new cluster. Returns 0 on hit (fills out_cluster/out_idx/out_was_end: slot's
// cluster / slot index / whether slot was originally the dir end) / -errno.
// Caller must hold fat_lock.
static int fat32_dir_find_slot(uint32_t start_cluster, uint32_t *out_cluster,
                               int *out_idx, int *out_was_end) {
  int entries = bytes_per_cluster / 32;
  int free_idx = -1;
  int was_end_of_dir = 0;
  uint32_t target_cluster = start_cluster;
  uint32_t tail = start_cluster;
  uint32_t cur = start_cluster;
  while (cur >= 2 && cur < 0x0FFFFFF8) {
    tail = cur;
    uint8_t *db = read_cluster_buf(cur);
    if (!db)
      return -EIO;
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
    if (nc == 0)
      return -ENOSPC;
    if (fat32_link_cluster(tail, nc) != 0) {
      fat32_write_fat_entry(nc, 0);
      return -EIO;
    }
    uint8_t *zb = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!zb) {
      fat32_write_fat_entry(nc, 0);
      fat32_write_fat_entry(tail, 0x0FFFFFFF);
      return -ENOMEM;
    }
    __memset(zb, 0, bytes_per_cluster);
    uint32_t lba = data_start_lba + (nc - 2) * sectors_per_cluster;
    partition_write(fat32_part, lba, sectors_per_cluster, zb);
    kfree(zb);
    target_cluster = nc;
    free_idx = 0;
    was_end_of_dir = 1;
  }
  *out_cluster = target_cluster;
  *out_idx = free_idx;
  *out_was_end = was_end_of_dir;
  return 0;
}

// fat32_dir_create: create regular file `name` in dir; returns +1 new inode or
// ERR_PTR(-errno).
static struct inode *fat32_dir_create(struct inode *dir, const char *name,
                                      int mode) {
  (void)mode;
  int namelen = 0;
  while (name[namelen])
    namelen++;
  if (namelen == 0)
    return ERR_PTR(-ENOENT);

  mutex_lock(&fat_lock);
  // Find a free slot (0x00 or 0xE5) on dir->start_cluster's chain; extend a
  // cluster if none.
  int entries = bytes_per_cluster / 32;
  uint32_t target_cluster;
  int free_idx, was_end_of_dir;
  int frc = fat32_dir_find_slot(FAT_I(dir)->start_cluster, &target_cluster,
                                &free_idx, &was_end_of_dir);
  if (frc != 0) {
    mutex_unlock(&fat_lock);
    return ERR_PTR(frc);
  }
  uint8_t *db = read_cluster_buf(target_cluster);
  if (!db) {
    mutex_unlock(&fat_lock);
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
  mutex_unlock(&fat_lock);
  if (wrc != 0)
    return ERR_PTR(-EIO);
  uint32_t ino = fat32_make_ino(target_cluster, free_idx);
  // IN_CREATE on the parent dir (fat_lock released at :621). inotify never
  // fails the VFS path; the child inode isn't watched yet so no self-event.
  inotify_inode_event(dir, IN_CREATE, 0, name);
  return fat32_iget(ino, INODE_REGULAR, 0, 0, target_cluster, free_idx);
}

// fat32_dir_mkdir: create subdirectory `name` in dir; returns 0 on success /
// -errno on failure.
static int fat32_dir_mkdir(struct inode *dir, const char *name, int mode) {
  (void)mode;
  int namelen = 0;
  while (name[namelen])
    namelen++;
  if (namelen == 0)
    return -ENOENT;
  mutex_lock(&fat_lock);
  uint32_t new_cluster = fat32_allocate_cluster();
  if (new_cluster == 0) {
    mutex_unlock(&fat_lock);
    return -ENOSPC;
  }
  uint8_t *db = (uint8_t *)kmalloc(bytes_per_cluster);
  if (!db) {
    fat32_write_fat_entry(new_cluster, 0);
    mutex_unlock(&fat_lock);
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
  uint32_t parent_cluster = FAT_I(dir)->start_cluster;
  if (parent_cluster == root_cluster)
    parent_cluster = 0;
  dd->fst_clus_hi = (parent_cluster >> 16) & 0xFFFF;
  dd->fst_clus_lo = parent_cluster & 0xFFFF;
  uint32_t lba = data_start_lba + (new_cluster - 2) * sectors_per_cluster;
  partition_write(fat32_part, lba, sectors_per_cluster, db);
  kfree(db);
  // Find a free slot (0x00 or 0xE5) on dir->start_cluster's chain; extend a
  // cluster if none (same as fat32_dir_create, via the shared helper).
  int entries = bytes_per_cluster / 32;
  uint32_t target_cluster;
  int free_idx, was_end_of_dir;
  int frc = fat32_dir_find_slot(FAT_I(dir)->start_cluster, &target_cluster,
                                &free_idx, &was_end_of_dir);
  if (frc != 0) {
    fat32_free_chain(new_cluster);
    mutex_unlock(&fat_lock);
    return frc;
  }
  uint8_t *pb = read_cluster_buf(target_cluster);
  if (!pb) {
    fat32_free_chain(new_cluster);
    mutex_unlock(&fat_lock);
    return -EIO;
  }
  struct fat_dir_entry ne;
  __memset(&ne, 0, sizeof(ne));
  format_83_name(name, namelen, ne.name);
  ne.attr = 0x10;
  ne.fst_clus_hi = (new_cluster >> 16) & 0xFFFF;
  ne.fst_clus_lo = new_cluster & 0xFFFF;
  ne.file_size = 0;
  __memcpy(pb + free_idx * 32, &ne, 32);
  // When the slot taken was the dir-end, zero the next slot as a chain-scan
  // terminator (same as fat32_dir_create).
  if (was_end_of_dir && free_idx + 1 < entries)
    __memset(pb + (free_idx + 1) * 32, 0, 32);
  int sec = (free_idx * 32) / 512;
  int wrc = write_cluster_sector(target_cluster, sec, pb + sec * 512);
  if (wrc == 0 && was_end_of_dir && free_idx + 1 < entries) {
    int ns = ((free_idx + 1) * 32) / 512;
    if (ns != sec)
      wrc = write_cluster_sector(target_cluster, ns, pb + ns * 512);
  }
  kfree(pb);
  mutex_unlock(&fat_lock);
  if (wrc != 0)
    return -EIO;
  // Pre-build the inode cache entry (caller sys_mkdir doesn't take the inode
  // back, only checks success). ino must be generated from the slot's actual
  // cluster target_cluster (consistent with lookup).
  uint32_t ino = fat32_make_ino(target_cluster, free_idx);
  struct inode *ip =
      fat32_iget(ino, INODE_DIR, 0, new_cluster, target_cluster, free_idx);
  if (ip)
    inode_put(ip); // cache holds the base ref; here we drop iget's +1
  // IN_CREATE on the parent dir (fat_lock released at :712).
  inotify_inode_event(dir, IN_CREATE, 0, name);
  return 0;
}

// fat32_dir_unlink: delete child file `name` from dir.
static int fat32_dir_unlink(struct inode *dir, const char *name) {
  uint32_t cluster, dir_cluster;
  int dir_idx, is_dir;
  uint64_t size;
  (void)size;
  int rc = fat32_lookup_in_dir(FAT_I(dir)->start_cluster, name, &cluster,
                               &dir_cluster, &dir_idx, &size, &is_dir);
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
  struct inode *ip =
      inode_lookup(&fat32_sb, fat32_make_ino(dir_cluster, dir_idx));
  if (ip) {
    fat32_invalidate_pages(ip);
    inode_put(ip);
  }
  db[dir_idx * 32] = 0xE5;
  int sec = (dir_idx * 32) / 512;
  write_cluster_sector(dir_cluster, sec, db + sec * 512);
  kfree(db);
  if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8) {
    mutex_lock(&fat_lock);
    fat32_free_chain(target_cluster);
    mutex_unlock(&fat_lock);
  }
  // IN_DELETE on the parent (name) + IN_DELETE_SELF on the child inode. No
  // fat_lock/i_lock held here. inode_lookup returns +1; inotify_inode_event
  // only reads the pointer as an index key (no ref needed), so drop the ref
  // here — otherwise every unlink leaks one inode ref, pinning the inode in
  // the cache. A later create reusing the same dir-entry slot (same ino) then
  // fat32_iget-hits the stale inode and reads the dead file's old size/data.
  inotify_inode_event(dir, IN_DELETE, 0, name);
  struct inode *child =
      inode_lookup(&fat32_sb, fat32_make_ino(dir_cluster, dir_idx));
  if (child) {
    inotify_inode_event(child, IN_DELETE_SELF, 0, NULL);
    inode_put(child);
  }
  return 0;
}

// fat32_dir_is_empty: scan the directory cluster chain; if only . / .. or all
// 0x00, treat as empty, return 1; else 0. Extracted from fat32_dir_rmdir's
// inline emptiness check, shared by rmdir/rename.
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
        return 1; // dir end, only . .. seen so far → empty
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

// fat32_dir_rmdir: delete empty subdirectory `name` from dir.
static int fat32_dir_rmdir(struct inode *dir, const char *name) {
  uint32_t cluster, dir_cluster;
  int dir_idx, is_dir;
  uint64_t size;
  int rc = fat32_lookup_in_dir(FAT_I(dir)->start_cluster, name, &cluster,
                               &dir_cluster, &dir_idx, &size, &is_dir);
  if (rc != 0)
    return rc;
  if (!is_dir)
    return -ENOTDIR;
  uint32_t target_cluster = cluster;
  if (target_cluster == 0)
    target_cluster = root_cluster;
  if (!fat32_dir_is_empty(target_cluster))
    return -EBUSY;
  mutex_lock(&fat_lock);
  uint8_t *db = read_cluster_buf(dir_cluster);
  if (!db) {
    mutex_unlock(&fat_lock);
    return -EIO;
  }
  db[dir_idx * 32] = 0xE5;
  int sec = (dir_idx * 32) / 512;
  write_cluster_sector(dir_cluster, sec, db + sec * 512);
  kfree(db);
  if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8)
    fat32_free_chain(target_cluster);
  mutex_unlock(&fat_lock);
  // IN_DELETE on the parent (name) + IN_DELETE_SELF on the removed dir inode.
  // inode_lookup returns +1; inotify_inode_event only reads the pointer as an
  // index key, so drop the ref here (same ref-leak fix as fat32_dir_unlink).
  inotify_inode_event(dir, IN_DELETE, 0, name);
  struct inode *child =
      inode_lookup(&fat32_sb, fat32_make_ino(dir_cluster, dir_idx));
  if (child) {
    inotify_inode_event(child, IN_DELETE_SELF, 0, NULL);
    inode_put(child);
  }
  return 0;
}

// fat32_getattr: fill kstat from inode fields. S08: report real
// ip->mode/uid/gid (set by sys_open/mkdir at creation); blksize = fat32 cluster
// size. FAT32 disk stores no permission bits; mode/uid/gid are in-memory only
// (revert to defaults on reboot). This OS is single-root, uid is mostly 0,
// acceptable (todo: a real permission FS).
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
  // Timestamps (Q5 in-memory): getattr splits ns into sec/nsec. FAT32 disk
  // stores no timestamps; inode fields are written by update_time/utimensat
  // (revert to 0 on reboot, acceptable).
  ks->st_atim.tv_sec = ip->atime.tv_sec;
  ks->st_atim.tv_nsec = ip->atime.tv_nsec;
  ks->st_mtim.tv_sec = ip->mtime.tv_sec;
  ks->st_mtim.tv_nsec = ip->mtime.tv_nsec;
  ks->st_ctim.tv_sec = ip->ctime.tv_sec;
  ks->st_ctim.tv_nsec = ip->ctime.tv_nsec;
  return 0;
}

// fat32_setattr: change inode size. Lock order i_lock → fat_lock (§6.6):
// setattr takes i_lock itself, then calls fat32_ftruncate (which takes
// fat_lock internally). Caller does not hold i_lock.
static int fat32_setattr(struct inode *ip, uint64_t size) {
  mutex_lock(&ip->i_lock);
  int rc = fat32_ftruncate(ip, size);
  mutex_unlock(&ip->i_lock);
  return rc;
}

// fat32_dir_rename: move the entry old_name under old_parent to new_name under
// new_parent. Strategy: find a target slot in new_parent (overwrite new's slot
// if new already exists, else find a free slot / extend a cluster), copy the
// old entry's 32B verbatim to the target slot and rewrite only the name as
// new_name's 8.3 form (attr/fst_clus/size unchanged → inode identity, i.e. the
// first cluster, unchanged), then mark the old slot 0xE5; on overwrite, free
// new's original chain. Lock order matches fat32_dir_create: fat_lock held
// throughout.
//
// POSIX semantics (mirrors Linux rename(2)):
//  - old doesn't exist → -ENOENT
//  - new doesn't exist → pure rename (entry migration, chain unchanged)
//  - new exists and is a file, old is a file → overwrite (free new's chain)
//  - new exists and is an empty dir, old is a dir → overwrite (free new's dir
//  chain)
//  - new exists and is a non-empty dir → -ENOTEMPTY
//  - old is a dir, new is a file → -EISDIR; old is a file, new is a dir →
//  -ENOTDIR
//  - old==new same name → 0 no-op
//  - same inode (old/new point to same first cluster) → 0 no-op (prevents
//    self-overwrite)
// Cross-directory dir moves need to update the moved dir's .. and guard against
// cycles; this implementation doesn't support that: old_parent != new_parent
// and old is a dir → -EXDEV. Same-directory rename leaves .. unchanged, so no
// cycle is possible.
static int fat32_dir_rename(struct inode *old_parent, const char *old_name,
                            struct inode *new_parent, const char *new_name) {
  if (!old_parent || !old_name || !new_parent || !new_name)
    return -EFAULT;

  // old/new same dir and same name → no-op (mirrors Linux).
  if (old_parent == new_parent && __strcmp(old_name, new_name) == 0)
    return 0;

  // Cross-directory dir moves need .. update + cycle guard; this implementation
  // doesn't support them (mirrors mvfat: return -EXDEV). Same-directory rename
  // leaves .. unchanged; cross-directory file moves also unsupported to keep
  // the boundary clear.
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
  int rc = fat32_lookup_in_dir(FAT_I(old_parent)->start_cluster, old_name,
                               &old_cluster, &old_dir_cluster, &old_dir_idx,
                               &old_size, &old_is_dir);
  if (rc != 0)
    return rc; // -ENOENT

  uint32_t new_cluster, new_dir_cluster;
  int new_dir_idx, new_is_dir;
  uint64_t new_size;
  (void)new_size;
  rc = fat32_lookup_in_dir(FAT_I(new_parent)->start_cluster, new_name,
                           &new_cluster, &new_dir_cluster, &new_dir_idx,
                           &new_size, &new_is_dir);
  int new_exists = (rc == 0);
  if (rc != 0 && rc != -ENOENT)
    return rc; // -EIO etc.

  // old/new point to the same first cluster → same inode; mirrors Linux
  // returning 0 (prevents "overwrite self" accidental deletion).
  if (new_exists && old_cluster == new_cluster && old_cluster >= 2 &&
      old_cluster < 0x0FFFFFF8)
    return 0;

  // Overwrite semantic boundary checks (when new exists).
  if (new_exists) {
    if (old_is_dir) {
      if (!new_is_dir)
        return -EISDIR; // old is a dir → new is a file
      uint32_t new_dir_cluster0 = new_cluster ? new_cluster : root_cluster;
      if (!fat32_dir_is_empty(new_dir_cluster0))
        return -ENOTEMPTY;
    } else {
      if (new_is_dir)
        return -ENOTDIR; // old is a file → new is a dir
    }
  }

  mutex_lock(&fat_lock);

  // Read the old entry (keep attr/fst_clus/size, only rewrite name to
  // new_name's 8.3 form).
  uint8_t *old_db = read_cluster_buf(old_dir_cluster);
  if (!old_db) {
    mutex_unlock(&fat_lock);
    return -EIO;
  }
  struct fat_dir_entry ne;
  __memcpy(&ne, old_db + old_dir_idx * 32, sizeof(ne));
  kfree(old_db);
  format_83_name(new_name, namelen, ne.name);

  // Pick the target slot: overwrite uses new's old slot; pure rename finds a
  // free slot (0x00/0xE5), extending a cluster if none.
  uint32_t target_cluster;
  int target_idx;
  int was_end_of_dir = 0;
  if (new_exists) {
    target_cluster = new_dir_cluster;
    target_idx = new_dir_idx;
  } else {
    int entries = bytes_per_cluster / 32;
    target_cluster = FAT_I(new_parent)->start_cluster;
    target_idx = -1;
    uint32_t tail = FAT_I(new_parent)->start_cluster;
    uint32_t cur = FAT_I(new_parent)->start_cluster;
    while (cur >= 2 && cur < 0x0FFFFFF8) {
      tail = cur;
      uint8_t *db = read_cluster_buf(cur);
      if (!db) {
        mutex_unlock(&fat_lock);
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
        mutex_unlock(&fat_lock);
        return -ENOSPC;
      }
      if (fat32_link_cluster(tail, nc) != 0) {
        fat32_write_fat_entry(nc, 0);
        mutex_unlock(&fat_lock);
        return -EIO;
      }
      uint8_t *zb = (uint8_t *)kmalloc(bytes_per_cluster);
      if (!zb) {
        fat32_write_fat_entry(nc, 0);
        fat32_write_fat_entry(tail, 0x0FFFFFFF);
        mutex_unlock(&fat_lock);
        return -ENOMEM;
      }
      __memset(zb, 0, bytes_per_cluster);
      uint32_t lba = data_start_lba + (nc - 2) * sectors_per_cluster;
      partition_write(fat32_part, lba, sectors_per_cluster, zb);
      kfree(zb);
      target_cluster = nc;
      target_idx = 0;
      was_end_of_dir = 1;
    }
  }

  // Write the target slot: copy the old entry (name already changed). Preserve
  // the end-of-dir marker.
  uint8_t *new_db = read_cluster_buf(target_cluster);
  if (!new_db) {
    mutex_unlock(&fat_lock);
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
    mutex_unlock(&fat_lock);
    return -EIO;
  }

  // Delete the old slot (mark 0xE5). In the overwrite case old slot ≠ new slot
  // (different names), mark independently.
  if (old_dir_cluster != target_cluster || old_dir_idx != target_idx) {
    uint8_t *odb = read_cluster_buf(old_dir_cluster);
    if (!odb) {
      mutex_unlock(&fat_lock);
      return -EIO;
    }
    odb[old_dir_idx * 32] = 0xE5;
    int osec = (old_dir_idx * 32) / 512;
    wrc = write_cluster_sector(old_dir_cluster, osec, odb + osec * 512);
    kfree(odb);
    if (wrc != 0) {
      mutex_unlock(&fat_lock);
      return -EIO;
    }
  }

  // Overwrite: free new's original chain. FAT32 has no nlink, so chain release
  // is data deletion.
  if (new_exists) {
    uint32_t nc = new_cluster;
    if (nc >= 2 && nc < 0x0FFFFFF8)
      fat32_free_chain(nc);
  }

  // inode cache sync: the old inode's ino changes with the dir-entry position,
  // but its content (first cluster/size) is unchanged. Invalidate its page
  // cache like unlink does; already-open fds still address via the first
  // cluster and are unaffected by the dir-entry position (FAT32 file I/O goes
  // through the first cluster, doesn't depend on ino's dir_idx). We don't
  // actively migrate the inode cache entry; the next lookup builds a new inode
  // via fat32_iget at the new position; the old inode is reaped when closed.
  struct inode *old_ip =
      inode_lookup(&fat32_sb, fat32_make_ino(old_dir_cluster, old_dir_idx));
  if (old_ip) {
    fat32_invalidate_pages(old_ip);
    inode_put(old_ip);
  }

  mutex_unlock(&fat_lock);
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

// fat32_mount_root: return the mount-point root inode (with inode_get, +1).
// Root ino = root_cluster. Goes through fat32_iget to install fat32_dir_iop —
// fixes the boot deadlock caused by the R1 stub missing i_op.
static struct inode *fat32_mount_root(struct mount_entry *m) {
  m->sb.s_op = &fat32_sops;
  m->sb.part = fat32_part;
  return fat32_iget(root_cluster, INODE_DIR, 0, root_cluster, root_cluster, -1);
}

// ==================== Path resolution (synchronous) ====================

static int fat32_resolve_path(const char *path, uint32_t *out_cluster,
                              uint32_t *out_dir_cluster, int *out_dir_entry_idx,
                              uint64_t *out_file_size, int is_parent) {
  if (!path || path[0] != '/')
    return -ENOENT;

  // Root directory.
  if (path[1] == '\0') {
    *out_cluster = root_cluster;
    *out_dir_cluster = root_cluster;
    *out_dir_entry_idx = -1;
    *out_file_size = 0;
    return 0;
  }

  // For is_parent mode: find last slash, resolve the parent path, then return
  // parent dir info.
  int leaf_len = 0;
  int parent_end = -1; // index of last '/' in path

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
      // Parent is root.
      *out_cluster = root_cluster;
      *out_dir_cluster = root_cluster;
      *out_dir_entry_idx = -1;
      *out_file_size = 0;
      return 0;
    }
    // Extract leaf name length after last slash (not stored, just advance past
    // it).
    const char *ls = path + parent_end + 1;
    while (ls[leaf_len] && leaf_len < 255) {
      leaf_len++;
    }
  }

  uint32_t cluster = root_cluster;
  uint32_t prev_dir_cluster = root_cluster;
  const char *p = path + 1; // skip leading '/'

  while (*p) {
    // Extract next path component.
    char component[256];
    int clen = 0;
    while (*p && *p != '/' && clen < 255)
      component[clen++] = *p++;
    component[clen] = '\0';
    if (*p == '/')
      p++;

    int is_last = (*p == '\0');

    // In is_parent mode: if the remaining path is past parent_end, treat this
    // component as the last one (it's the parent directory).
    int is_parent_last = 0;
    if (is_parent && (p - path) > parent_end) {
      is_parent_last = 1;
    }

    // Handle "." and "..".
    if (component[0] == '.' && component[1] == '\0') {
      // "." — stay in current directory.
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
      // ".." — go to parent directory. Read the ".." entry to find the parent
      // cluster. For root, ".." points to root itself.
      if (cluster != root_cluster) {
        // Scan current dir for ".." entry to get parent cluster.
        uint32_t parent_cluster = root_cluster; // default
        uint32_t scan2 = cluster;
        while (scan2 >= 2 && scan2 < 0x0FFFFFF8) {
          uint32_t lba2 = data_start_lba + (scan2 - 2) * sectors_per_cluster;
          uint8_t *buf2 = (uint8_t *)kmalloc(bytes_per_cluster);
          if (!buf2)
            return -ENOMEM;
          if (partition_read(fat32_part, lba2, sectors_per_cluster, buf2) !=
              0) {
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
      // else: already at root, stay.
      if (is_last || is_parent_last) {
        *out_cluster = cluster;
        *out_dir_cluster = prev_dir_cluster;
        *out_dir_entry_idx = -1;
        *out_file_size = 0;
        return 0;
      }
      continue;
    }

    // Scan current directory for this component.
    int found = 0;
    uint32_t scan_cluster = cluster;
    char lfn_buf[256];
    __memset(lfn_buf, 0, sizeof(lfn_buf));

    while (scan_cluster >= 2 && scan_cluster < 0x0FFFFFF8) {
      uint32_t lba = data_start_lba + (scan_cluster - 2) * sectors_per_cluster;
      uint8_t *buf = (uint8_t *)kmalloc(bytes_per_cluster);
      if (!buf)
        return -ENOMEM;
      if (partition_read(fat32_part, lba, sectors_per_cluster, buf) != 0) {
        kfree(buf);
        return -EIO;
      }

      int entries = bytes_per_cluster / 32;
      for (int i = 0; i < entries; i++) {
        struct fat_dir_entry *de = (struct fat_dir_entry *)(buf + i * 32);
        if (de->name[0] == 0x00) {
          // End of directory.
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

        // Short name entry — check match.
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
            // Found the target.
            *out_cluster = entry_cluster;
            *out_dir_cluster = scan_cluster;
            *out_dir_entry_idx = i;
            *out_file_size = de->file_size;
            kfree(buf);
            return 0;
          }

          // Intermediate component — descend.
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

      // Follow FAT chain.
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

// ==================== Directory entry read/write helpers ====================

// Read a cluster from disk into a kmalloc'd buffer.
static uint8_t *read_cluster_buf(uint32_t cluster) {
  uint8_t *buf = (uint8_t *)kmalloc(bytes_per_cluster);
  if (!buf)
    return NULL;
  uint32_t lba = data_start_lba + (cluster - 2) * sectors_per_cluster;
  if (partition_read(fat32_part, lba, sectors_per_cluster, buf) != 0) {
    kfree(buf);
    return NULL;
  }
  return buf;
}

// Write back a single sector from a cluster buffer.
static int write_cluster_sector(uint32_t cluster, int sector_idx,
                                const uint8_t *data) {
  uint32_t lba =
      data_start_lba + (cluster - 2) * sectors_per_cluster + sector_idx;
  return partition_write(fat32_part, lba, 1, data);
}

// Update directory entry on disk.
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

// ==================== Truncate (free cluster chain, zero size)
// ====================

static int fat32_truncate(uint32_t cluster, uint32_t dir_cluster, int dir_idx) {
  mutex_lock(&fat_lock);
  fat32_free_chain(cluster);
  mutex_unlock(&fat_lock);

  // Update directory entry: cluster=0, size=0.
  return fat32_update_dir_entry(dir_cluster, dir_idx, 0, 0);
}

// ==================== ftruncate to an arbitrary length ====================
// Grow or shrink an existing regular file to exactly len bytes. New bytes (when
// growing) read back as zero. Updates the inode size + on-disk dir entry and
// invalidates the page cache so subsequent reads see the new length.
// Caller holds ip->i_lock.
int fat32_ftruncate(struct inode *ip, uint64_t len) {
  if (ip->type != INODE_REGULAR)
    return -EINVAL;

  uint32_t bpc = fat32_bytes_per_cluster();
  if (bpc == 0)
    return -EIO;

  // No work if size already matches.
  if (ip->size == len)
    return 0;

  if (len == 0) {
    // Shrink-to-zero reuses the simple path.
    int rc =
        fat32_truncate(FAT_I(ip)->start_cluster, FAT_I(ip)->dir_start_cluster,
                       FAT_I(ip)->dir_entry_index);
    if (rc)
      return rc;
    FAT_I(ip)->start_cluster = 0;
    ip->size = 0;
    fat32_invalidate_pages(ip);
    return 0;
  }

  uint32_t keep_clusters = (uint32_t)((len + bpc - 1) / bpc);

  if (len < ip->size) {
    // Shrink: free every cluster past the keep boundary.
    mutex_lock(&fat_lock);
    if (FAT_I(ip)->start_cluster == 0) {
      mutex_unlock(&fat_lock);
      return -EIO;
    }
    // Walk to the last cluster we keep (index keep_clusters-1).
    uint32_t c = FAT_I(ip)->start_cluster;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < keep_clusters; i++) {
      if (c < 2 || c >= 0x0FFFFFF8) {
        // Chain shorter than keep_clusters — nothing to free.
        c = 0;
        break;
      }
      prev = c;
      c = fat32_read_entry(c);
    }
    if (prev != 0 && c >= 2 && c < 0x0FFFFFF8) {
      // Terminate the kept chain and free the rest.
      fat32_write_fat_entry(prev, 0x0FFFFFFF);
      fat32_free_chain(c);
    }
    mutex_unlock(&fat_lock);
  } else {
    // Grow into a detached chain, zero it in physical extents, then publish
    // the single tail link only after all allocation and I/O has succeeded.
    mutex_lock(&fat_lock);
    uint32_t tail = FAT_I(ip)->start_cluster;
    uint32_t have = tail ? 1 : 0;
    uint32_t guard = total_data_clusters;
    while (tail && guard-- > 0) {
      uint32_t next = fat32_read_entry(tail);
      if (next >= 0x0FFFFFF8)
        break;
      tail = next;
      have++;
    }
    if (have < keep_clusters) {
      uint32_t detached_start = 0, detached_tail = 0;
      int rc = fat32_allocate_detached(keep_clusters - have, &detached_start,
                                       &detached_tail);
      if (rc) {
        mutex_unlock(&fat_lock);
        return rc;
      }
      if (tail) {
        rc = fat32_link_cluster(tail, detached_start);
        if (rc) {
          fat32_free_chain(detached_start);
          mutex_unlock(&fat_lock);
          return rc;
        }
      } else {
        FAT_I(ip)->start_cluster = detached_start;
      }
    }
    mutex_unlock(&fat_lock);
  }

  ip->size = len;
  if (FAT_I(ip)->dir_start_cluster >= 2) {
    fat32_update_dir_entry(FAT_I(ip)->dir_start_cluster,
                           FAT_I(ip)->dir_entry_index, FAT_I(ip)->start_cluster,
                           (uint32_t)ip->size);
  }
  fat32_invalidate_pages(ip);
  return 0;
}

// ==================== FAT32 init ====================

int fat32_init(struct block_partition *part) {
  printk(LOG_INFO, "fat32_init: starting\n");

  if (!part || (part->mbr_type != 0x0b && part->mbr_type != 0x0c)) {
    printk(LOG_ERROR, "fat32_init: root partition type=%x, expected 0b/0c\n",
           part ? part->mbr_type : 0);
    return -EINVAL;
  }
  fat32_part = part;
  fat32_sb.s_op = &fat32_sops;
  fat32_sb.part = part;
  fat32_sb.block_size = 512;
  fat32_sb.readonly = false;

  mutex_init(&fat_lock);
  mutex_init(&fat_cache_fill_lock);

  // Initialize FAT cache.
  for (int i = 0; i < FAT_CACHE_PAGES; i++) {
    fat_cache[i].sector_lba = 0xFFFFFFFF;
    fat_cache[i].generation = 0;
    fat_cache[i].filling = false;
  }

  part_start_lba = 0;
  uint64_t part_total_sectors64 = part->sector_count;
  if (part_total_sectors64 > UINT32_MAX)
    return -EOVERFLOW;
  uint32_t part_total_sectors = (uint32_t)part_total_sectors64;

  // Read BPB.
  uint8_t bpb[512];
  if (partition_read(part, 0, 1, bpb) != 0) {
    printk(LOG_ERROR, "fat32_init: BPB read failed\n");
    return -EIO;
  }

  uint16_t bps = (uint16_t)bpb[11] | ((uint16_t)bpb[12] << 8);
  sectors_per_cluster = bpb[13];
  uint16_t reserved = (uint16_t)bpb[14] | ((uint16_t)bpb[15] << 8);
  uint16_t bpb_fsinfo = (uint16_t)bpb[48] | ((uint16_t)bpb[49] << 8);
  uint16_t backup_boot = (uint16_t)bpb[50] | ((uint16_t)bpb[51] << 8);
  spf32 = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
          ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
  root_cluster = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                 ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);

  if (bps != 512 || sectors_per_cluster == 0 ||
      (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
      sectors_per_cluster > 128 || spf32 == 0 || root_cluster < 2) {
    printk(LOG_ERROR,
           "fat32_init: invalid BPB (bps=%u spc=%u spf=%u root=%u)\n", bps,
           sectors_per_cluster, spf32, root_cluster);
    return -EINVAL;
  }

  fat_start_lba = reserved;
  data_start_lba = fat_start_lba + spf32 * 2;
  bytes_per_cluster = sectors_per_cluster * 512;

  if (part_total_sectors > 0) {
    uint32_t data_sectors =
        part_total_sectors - (data_start_lba - part_start_lba);
    total_data_clusters = data_sectors / sectors_per_cluster;
  } else {
    total_data_clusters = 0;
  }

  fsinfo_valid = false;
  fsinfo_dirty = false;
  fsinfo_free_clusters = UINT32_MAX;
  fsinfo_sector = bpb_fsinfo;
  fsinfo_backup_sector = UINT32_MAX;
  next_free_hint = 2;
  if (bpb_fsinfo != 0 && bpb_fsinfo != 0xFFFF && bpb_fsinfo < reserved) {
    uint8_t fsinfo[512];
    if (partition_read(part, fsinfo_sector, 1, fsinfo) == 0 &&
        fat32_fsinfo_signatures_valid(fsinfo)) {
      uint32_t free_clusters = fat32_load_u32(fsinfo + 488);
      uint32_t next = fat32_load_u32(fsinfo + 492);
      if (free_clusters == UINT32_MAX || free_clusters <= total_data_clusters) {
        fsinfo_free_clusters = free_clusters;
        fsinfo_valid = true;
      }
      if (next >= 2 && next < total_data_clusters + 2)
        next_free_hint = next;
      if (backup_boot != 0 && backup_boot != 0xFFFF &&
          (uint32_t)backup_boot + bpb_fsinfo < reserved &&
          (uint32_t)backup_boot + bpb_fsinfo != fsinfo_sector)
        fsinfo_backup_sector = (uint32_t)backup_boot + bpb_fsinfo;
    }
  }
  if (!fsinfo_valid)
    printk(LOG_WARN, "fat32: valid FSInfo free-cluster summary unavailable\n");

  // Keep the beginning of the FAT resident. Warming the whole table only
  // evicts its useful prefix when the table is larger than the cache.
  uint32_t prewarm_sectors = spf32 < FAT_CACHE_PAGES ? spf32 : FAT_CACHE_PAGES;
  for (uint32_t s = 0; s < prewarm_sectors; s++) {
    fat_cache_read(fat_start_lba + s);
  }

  printk(LOG_INFO,
         "fat32_init: part=%u fat=%u data=%u root=%u spc=%u bpc=%u total_cl=%u "
         "free=%u next=%u\n",
         part_start_lba, fat_start_lba, data_start_lba, root_cluster,
         sectors_per_cluster, bytes_per_cluster, total_data_clusters,
         fsinfo_free_clusters, next_free_hint);
  return 0;
}

void fat32_dump_cache_stats(void) {
  uint64_t hits = __atomic_load_n(&fat_cache_hits, __ATOMIC_RELAXED);
  uint64_t misses = __atomic_load_n(&fat_cache_misses, __ATOMIC_RELAXED);
  uint64_t accesses = hits + misses;
  uint64_t hit_permille = accesses ? hits * 1000 / accesses : 0;
  printk(LOG_DEBUG,
         "fat32-cache: hits=%lu misses=%lu hit=%lu.%lu%% waits=%lu "
         "commands=%lu sectors=%lu\n",
         hits, misses, hit_permille / 10, hit_permille % 10,
         __atomic_load_n(&fat_cache_fill_waits, __ATOMIC_RELAXED),
         __atomic_load_n(&fat_cache_io_commands, __ATOMIC_RELAXED),
         __atomic_load_n(&fat_cache_io_sectors, __ATOMIC_RELAXED));
  printk(LOG_DEBUG,
         "fat32-alloc: batches=%lu sector_reads=%lu fat1_writes=%lu "
         "fat2_writes=%lu zero_cmds=%lu\n",
         __atomic_load_n(&fat_batch_count, __ATOMIC_RELAXED),
         __atomic_load_n(&fat_sector_reads, __ATOMIC_RELAXED),
         __atomic_load_n(&fat1_writes, __ATOMIC_RELAXED),
         __atomic_load_n(&fat2_writes, __ATOMIC_RELAXED),
         __atomic_load_n(&fat_zero_commands, __ATOMIC_RELAXED));
  struct blk_stats bs;
  blk_get_stats(&bs);
  printk(LOG_DEBUG,
         "blk: submitted=%lu completed=%lu failed=%lu read_cmds=%lu "
         "write_cmds=%lu read_sectors=%lu write_sectors=%lu\n",
         bs.submitted, bs.completed, bs.failed, bs.read_cmds, bs.write_cmds,
         bs.read_sectors, bs.write_sectors);
  struct page_cache_stats pcs;
  page_cache_get_stats(&pcs);
  printk(LOG_DEBUG,
         "readahead: batches=%lu pages=%lu hits=%lu waste=%lu "
         "fragment_trunc=%lu fallback=%lu\n",
         pcs.readahead_batches, pcs.readahead_pages, pcs.readahead_hits,
         pcs.readahead_waste, pcs.readahead_fragment_truncations,
         pcs.readahead_fallbacks);
}

// ==================== File read ====================

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

    struct cache_page *cp;
    uint32_t read_window = 1;
    if (count - nread >= 4 * 4096) {
      uint64_t bytes = page_off + (count - nread);
      read_window = (uint32_t)((bytes + 4095) / 4096);
      if (read_window > PAGE_CACHE_RA_MAX_PAGES)
        read_window = PAGE_CACHE_RA_MAX_PAGES;
    }
    int rc =
        page_cache_get_ra(ip, page_idx, read_window, PAGE_CACHE_RA_READ, &cp);
    if (rc)
      return nread ? (int)nread : rc;
    __memcpy((uint8_t *)buf + nread, cp->data + page_off, chunk);
    page_cache_release(cp);
    nread += chunk;
  }
  return (int)nread;
}

// ==================== File write ====================

int fat32_write(struct inode *ip, uint64_t offset, const void *buf,
                size_t count) {
  mutex_lock(&ip->i_lock);
  uint64_t original_size = ip->size;

  // O_APPEND: write at end of file.
  if (ip->mode & O_APPEND) {
    offset = ip->size;
  }

  size_t written = 0;
  size_t durable = 0;
  struct cache_page *write_pages[16];
  int nr_write_pages = 0;
  int writeback_error = 0;
  int operation_error = 0;
  uint32_t append_tail = 0;
  while (written < count) {
    uint64_t page_idx = (offset + written) / 4096;
    uint32_t page_off = (offset + written) % 4096;
    uint32_t chunk = 4096 - page_off;
    if (chunk > count - written)
      chunk = count - written;

    // The page cache is 4KB-granular: page_cache_fill reads and
    // page_cache_writeback writes every cluster spanning the page
    // (clusters_per_page of them — 8 when the FAT32 cluster is 512B). For the
    // writeback to persist the whole page, every cluster the write touches must
    // already exist in the chain; allocating only the first one (as the old
    // code did) leaves the rest unallocated, so writeback walks the chain, hits
    // EOF after the first cluster, and bails — silently dropping everything
    // past the first cluster and corrupting files >4KB. Allocate every cluster
    // covering [page_off, page_off+chunk) here, in order, tail-linked onto the
    // chain.
    uint64_t write_start = offset + written;
    uint64_t write_end = write_start + chunk - 1;
    uint32_t first_chain = (uint32_t)(write_start / bytes_per_cluster);
    uint32_t last_chain = (uint32_t)(write_end / bytes_per_cluster);
    bool enospc = false;
    for (uint32_t chain_index = first_chain; chain_index <= last_chain;
         chain_index++) {
      uint32_t existing =
          fat32_walk_chain(FAT_I(ip)->start_cluster, chain_index);
      if (existing >= 2 && existing < 0x0FFFFFF8)
        continue; // already allocated

      // Sequential extension already has the preceding logical cluster. Keep
      // that tail across this write instead of rescanning the entire FAT chain
      // for every new 512-byte cluster (and imposing an artificial 1024-entry
      // file-size ceiling).
      if (FAT_I(ip)->start_cluster != 0 && append_tail == 0 && chain_index != 0)
        append_tail =
            fat32_walk_chain(FAT_I(ip)->start_cluster, chain_index - 1);
      if (FAT_I(ip)->start_cluster != 0 &&
          (append_tail < 2 || append_tail >= 0x0FFFFFF8)) {
        WARN_ON(1);
        mutex_unlock(&ip->i_lock);
        return written ? (int)written : -EIO;
      }

      uint32_t need = last_chain - chain_index + 1;
      mutex_lock(&fat_lock);
      uint32_t detached_start = 0, detached_tail = 0;
      int alloc_rc =
          fat32_allocate_detached(need, &detached_start, &detached_tail);
      if (alloc_rc != 0 ||
          (FAT_I(ip)->start_cluster != 0 &&
           fat32_link_cluster(append_tail, detached_start) != 0)) {
        operation_error = alloc_rc ? alloc_rc : -EIO;
        if (detached_start)
          fat32_free_chain(detached_start);
        mutex_unlock(&fat_lock);
        enospc = true;
        break;
      }
      if (FAT_I(ip)->start_cluster == 0)
        FAT_I(ip)->start_cluster = detached_start;
      append_tail = detached_tail;
      mutex_unlock(&fat_lock);

      // Invalidate page cache — new cluster changes mapping.
      fat32_invalidate_pages(ip);
      break;
    }
    if (enospc)
      break;

    struct cache_page *cp = page_cache_fill(ip, page_idx);
    if (!cp) {
      operation_error = -EIO;
      break;
    }
    __memcpy(cp->data + page_off, (const uint8_t *)buf + written, chunk);
    page_cache_mark_dirty(cp);
    write_pages[nr_write_pages++] = cp;
    written += chunk;

    /* writepages limits I/O using i_size. Publish the bytes covered by this
     * dirty page before writeback, then roll back to the durable prefix if
     * writeback fails below. */
    uint64_t pending_end = offset + written;
    if (pending_end > ip->size)
      ip->size = pending_end;

    if (nr_write_pages == 16 || written == count) {
      writeback_error = page_cache_writeback_pages(write_pages, nr_write_pages);
      for (int i = 0; i < nr_write_pages; i++)
        page_cache_release(write_pages[i]);
      nr_write_pages = 0;
      if (writeback_error)
        break;
      durable = written;
    }
  }

  if (nr_write_pages != 0) {
    writeback_error = page_cache_writeback_pages(write_pages, nr_write_pages);
    for (int i = 0; i < nr_write_pages; i++)
      page_cache_release(write_pages[i]);
    if (!writeback_error)
      durable = written;
  }
  if (writeback_error)
    written = durable;
  if (writeback_error)
    operation_error = writeback_error;

  uint64_t new_end = offset + written;
  uint64_t final_size = new_end > original_size ? new_end : original_size;
  if (writeback_error)
    ip->size = final_size;
  if (final_size != original_size) {
    ip->size = final_size;
    // Update directory entry.
    if (FAT_I(ip)->dir_start_cluster >= 2) {
      fat32_update_dir_entry(FAT_I(ip)->dir_start_cluster,
                             FAT_I(ip)->dir_entry_index,
                             FAT_I(ip)->start_cluster, (uint32_t)ip->size);
    }
  }

  mutex_unlock(&ip->i_lock);
  // IN_MODIFY on the written file (i_lock released above; fat_lock not held
  // here). Only fire if something was actually written.
  if (written > 0)
    inotify_inode_event(ip, IN_MODIFY, 0, NULL);
  return written ? (int)written : operation_error;
}

// ==================== Mkdir ====================

int fat32_mkdir(const char *path) {
  // Resolve parent directory.
  uint32_t parent_cluster, dummy_cluster;
  int dummy_idx;
  uint64_t dummy_size;
  int rc = fat32_resolve_path(path, &dummy_cluster, &parent_cluster, &dummy_idx,
                              &dummy_size, 1);
  if (rc != 0)
    return rc;

  // Extract leaf name.
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

  // Allocate a cluster for the new directory.
  uint32_t new_cluster = fat32_allocate_cluster();
  if (new_cluster == 0)
    return -ENOSPC;

  // Zero-fill the cluster.
  uint8_t *dir_buf = (uint8_t *)kmalloc(bytes_per_cluster);
  if (!dir_buf) {
    fat32_write_fat_entry(new_cluster, 0);
    return -ENOMEM;
  }
  __memset(dir_buf, 0, bytes_per_cluster);

  // Create "." and ".." entries.
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
  uint32_t dotdot_cluster = parent_cluster;
  if (dotdot_cluster == root_cluster)
    dotdot_cluster = 0;
  dotdot->fst_clus_hi = (dotdot_cluster >> 16) & 0xFFFF;
  dotdot->fst_clus_lo = dotdot_cluster & 0xFFFF;

  // Write new directory cluster to disk.
  uint32_t lba = data_start_lba + (new_cluster - 2) * sectors_per_cluster;
  partition_write(fat32_part, lba, sectors_per_cluster, dir_buf);
  kfree(dir_buf);

  // Add entry in parent directory.
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

// ==================== Unlink ====================

int fat32_unlink(const char *path) {
  uint32_t cluster, dir_cluster;
  int dir_idx;
  uint64_t file_size;

  int rc =
      fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx, &file_size, 0);
  if (rc != 0)
    return rc;

  // Read directory entry to check it's not a directory.
  uint8_t *dir_buf = read_cluster_buf(dir_cluster);
  if (!dir_buf)
    return -EIO;
  struct fat_dir_entry *de = (struct fat_dir_entry *)(dir_buf + dir_idx * 32);

  if (de->attr & 0x10) {
    kfree(dir_buf);
    return -EISDIR;
  }

  uint32_t target_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;

  // Invalidate page cache for this inode (lookup by dir-entry location ino).
  struct inode *ip =
      inode_lookup(&fat32_sb, fat32_make_ino(dir_cluster, dir_idx));
  if (ip) {
    fat32_invalidate_pages(ip);
    inode_put(ip);
  }

  // Mark entry as deleted (0xE5).
  dir_buf[dir_idx * 32] = 0xE5;
  int sector_idx = (dir_idx * 32) / 512;
  write_cluster_sector(dir_cluster, sector_idx, dir_buf + sector_idx * 512);
  kfree(dir_buf);

  // Free cluster chain.
  if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8) {
    mutex_lock(&fat_lock);
    fat32_free_chain(target_cluster);
    mutex_unlock(&fat_lock);
  }

  return 0;
}

// ==================== Rmdir ====================

int fat32_rmdir(const char *path) {
  uint32_t cluster, dir_cluster;
  int dir_idx;
  uint64_t file_size;

  int rc =
      fat32_resolve_path(path, &cluster, &dir_cluster, &dir_idx, &file_size, 0);
  if (rc != 0)
    return rc;

  // Check it's a directory.
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

  // Check directory is empty (only . and ..).
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

  // Mark entry deleted in parent.
  dir_buf[dir_idx * 32] = 0xE5;
  int sector_idx = (dir_idx * 32) / 512;
  write_cluster_sector(dir_cluster, sector_idx, dir_buf + sector_idx * 512);
  kfree(dir_buf);

  // Free directory cluster chain.
  if (target_cluster >= 2 && target_cluster < 0x0FFFFFF8) {
    mutex_lock(&fat_lock);
    fat32_free_chain(target_cluster);
    mutex_unlock(&fat_lock);
  }

  return 0;
}

// ==================== Stat ====================

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
    // Reached via root, "." or ".." — no parent dir entry; treat as directory.
    st->st_mode = 0040755;
    st->st_nlink = 1;
    st->st_size = 0;
    st->st_blksize = bytes_per_cluster;
    return 0;
  }

  // Read directory entry for attributes.
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

// ==================== getdents: read directory entries into user buffer
// ==================== Each entry: struct dirent64 — defined in xos/dirent.h.
// pos tracks how many dir entries we've consumed so far.
// Returns total bytes written, or negative errno.

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
        // End of directory.
        kfree(cbuf);
        *pos = (uint64_t)-1; // signal EOF
        return (int)written;
      }
      if (de->name[0] == 0xE5) {
        lfn_buf[0] = '\0';
        continue;
      }
      // LFN entries precede their owning 8.3 entry, often as a sequence of
      // multiple records. Preserve the assembled name until that entry is
      // emitted; volume labels are unrelated and reset the assembly.
      if (de->attr == 0x0F) {
        collect_lfn_entry(de, lfn_buf);
        continue;
      }
      if (de->attr & 0x08) {
        lfn_buf[0] = '\0';
        continue;
      }

      // Skip entries we've already consumed (for resuming after pos).
      if (entry_idx < *pos) {
        entry_idx++;
        lfn_buf[0] = '\0';
        continue;
      }

      // Build name.
      char name[256];
      if (lfn_buf[0] != '\0') {
        int j;
        for (j = 0; lfn_buf[j] && j < 255; j++)
          name[j] = lfn_buf[j];
        name[j] = '\0';
      } else {
        // Convert 8.3 name.
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

      // Compute entry size: dirent64 header + name + null + padding to 8-byte
      // align.
      int name_len = 0;
      while (name[name_len])
        name_len++;
      uint16_t reclen = (uint16_t)(sizeof(struct dirent64) + name_len + 1);
      reclen = (reclen + 7) & ~7; // 8-byte align

      // If this entry doesn't fit, stop here.
      if (written + reclen > len) {
        kfree(cbuf);
        *pos = entry_idx;
        return (int)written;
      }

      // Fill entry.
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

    // Follow FAT chain.
    uint32_t next = fat32_read_entry(scan_cluster);
    if (next >= 0x0FFFFFF8) {
      *pos = (uint64_t)-1; // EOF
      return (int)written;
    }
    scan_cluster = next;
  }

  *pos = (uint64_t)-1;
  return (int)written;
}

// ==================== FAT32 fstype shims ====================
// fat32_resolve_path requires path[0]=='/', so relpath callbacks prepend '/'.
// fat32_getdents takes (dir_cluster, &pos, buf, len); shim extracts
// dir->start_cluster and passes &ctx->pos.

#include "kernel/bsd/mount.h"

static ssize_t fat32_fs_getdents(struct inode *dir, struct dir_context *ctx) {
  int ret =
      fat32_getdents(FAT_I(dir)->start_cluster, &ctx->pos, ctx->buf, ctx->len);
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
