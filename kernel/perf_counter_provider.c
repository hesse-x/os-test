/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/kernel.h"

#ifdef PERF

#include "kernel/bsd/fat32.h"
#include "kernel/bsd/page_cache.h"
#include "kernel/driver/ahci.h"
#include "kernel/driver/blk_dev.h"
#include "kernel/xcore/perf/counter.h"

static void
collect_external_counters(const struct perf_counter_writer *writer) {
  struct blk_stats block;
  blk_get_stats(&block);
  writer->set_available(writer->context, PERF_COUNTER_PROVIDER_BLOCK);
  writer->add(writer->context, PERF_COUNTER_BLOCK_SUBMITTED, block.submitted);
  writer->add(writer->context, PERF_COUNTER_BLOCK_COMPLETED, block.completed);
  writer->add(writer->context, PERF_COUNTER_BLOCK_FAILED, block.failed);
  writer->add(writer->context, PERF_COUNTER_BLOCK_REJECTED,
              block.validation_rejected);
  writer->add(writer->context, PERF_COUNTER_BLOCK_READ_CMDS, block.read_cmds);
  writer->add(writer->context, PERF_COUNTER_BLOCK_WRITE_CMDS, block.write_cmds);
  writer->add(writer->context, PERF_COUNTER_BLOCK_READ_SECTORS,
              block.read_sectors);
  writer->add(writer->context, PERF_COUNTER_BLOCK_WRITE_SECTORS,
              block.write_sectors);
  for (unsigned i = 0; i < 5; i++)
    writer->add(writer->context, PERF_COUNTER_BLOCK_READ_BUCKET_1 + i,
                block.read_size_buckets[i]);
  for (unsigned i = 0; i < 5; i++)
    writer->add(writer->context, PERF_COUNTER_BLOCK_WRITE_BUCKET_1 + i,
                block.write_size_buckets[i]);

  struct fat32_stats fat;
  fat32_get_stats(&fat);
  writer->set_available(writer->context, PERF_COUNTER_PROVIDER_FAT);
  writer->add(writer->context, PERF_COUNTER_FAT_CACHE_HITS, fat.cache_hits);
  writer->add(writer->context, PERF_COUNTER_FAT_CACHE_MISSES, fat.cache_misses);
  writer->add(writer->context, PERF_COUNTER_FAT_CACHE_WAITS,
              fat.cache_fill_waits);
  writer->add(writer->context, PERF_COUNTER_FAT_CACHE_COMMANDS,
              fat.cache_io_commands);
  writer->add(writer->context, PERF_COUNTER_FAT_CACHE_SECTORS,
              fat.cache_io_sectors);
  for (unsigned source = 0; source < 2; source++) {
    uint16_t base =
        source ? PERF_COUNTER_FAT_RA_CALLS : PERF_COUNTER_FAT_DEMAND_CALLS;
    writer->add(writer->context, base, fat.walk_calls[source]);
    writer->add(writer->context, base + 1, fat.walk_steps[source]);
    writer->add(writer->context, base + 2, fat.walk_head_restarts[source]);
    writer->add(writer->context, base + 3, fat.walk_backtracks[source]);
    writer->add(writer->context, base + 4, fat.walk_invalid[source]);
    writer->add(writer->context, base + 5, fat.mapped_sectors[source]);
  }

  struct page_cache_stats ra;
  page_cache_get_stats(&ra);
  writer->set_available(writer->context, PERF_COUNTER_PROVIDER_READAHEAD);
  writer->add(writer->context, PERF_COUNTER_RA_BATCHES, ra.readahead_batches);
  writer->add(writer->context, PERF_COUNTER_RA_PAGES, ra.readahead_pages);
  writer->add(writer->context, PERF_COUNTER_RA_HITS, ra.readahead_hits);
  writer->add(writer->context, PERF_COUNTER_RA_WASTE, ra.readahead_waste);
  writer->add(writer->context, PERF_COUNTER_RA_FRAGMENT_TRUNCATIONS,
              ra.readahead_fragment_truncations);
  writer->add(writer->context, PERF_COUNTER_RA_FALLBACKS,
              ra.readahead_fallbacks);

  struct ahci_stats ahci;
  ahci_get_stats(&ahci);
  writer->set_available(writer->context, PERF_COUNTER_PROVIDER_AHCI);
  writer->add(writer->context, PERF_COUNTER_AHCI_SYNC_SUBMITTED,
              ahci.sync_submitted);
  writer->add(writer->context, PERF_COUNTER_AHCI_ASYNC_SUBMITTED,
              ahci.async_submitted);
  writer->add(writer->context, PERF_COUNTER_AHCI_COMPLETED, ahci.completed);
  writer->add(writer->context, PERF_COUNTER_AHCI_ERRORS, ahci.errors);
  writer->add(writer->context, PERF_COUNTER_AHCI_SYNC_WAKES, ahci.sync_wakes);
  writer->add(writer->context, PERF_COUNTER_AHCI_ASYNC_WAKES, ahci.async_wakes);
  writer->add(writer->context, PERF_COUNTER_AHCI_EARLY_COMPLETES,
              ahci.early_completes);
  writer->add(writer->context, PERF_COUNTER_AHCI_CROSS_CPU_WAKES,
              ahci.cross_cpu_wakes);
  writer->add(writer->context, PERF_COUNTER_AHCI_QUEUE_FULL, ahci.queue_full);
  writer->add(writer->context, PERF_COUNTER_AHCI_INVALID_TIMING,
              ahci.invalid_timing);
  writer->add(writer->context, PERF_COUNTER_AHCI_QUEUE_WAIT_COUNT,
              ahci.queue_wait_count);
  writer->add(writer->context, PERF_COUNTER_AHCI_QUEUE_WAIT_CYCLES,
              ahci.queue_wait_cycles);
  writer->add(writer->context, PERF_COUNTER_AHCI_QUEUE_WAIT_MAX,
              ahci.queue_wait_max);
  writer->add(writer->context, PERF_COUNTER_AHCI_SERVICE_COUNT,
              ahci.service_count);
  writer->add(writer->context, PERF_COUNTER_AHCI_SERVICE_CYCLES,
              ahci.service_cycles);
  writer->add(writer->context, PERF_COUNTER_AHCI_SERVICE_MAX, ahci.service_max);
  writer->add(writer->context, PERF_COUNTER_AHCI_DEPTH_CYCLES,
              ahci.queue_depth_cycles);
  writer->add(writer->context, PERF_COUNTER_AHCI_DEPTH, ahci.queue_depth);
  writer->add(writer->context, PERF_COUNTER_AHCI_DEPTH_MAX,
              ahci.queue_depth_max);
  for (unsigned bucket = 0; bucket < AHCI_TIMING_BUCKETS; bucket++) {
    writer->add(writer->context,
                PERF_COUNTER_AHCI_QUEUE_WAIT_HIST_BASE + bucket,
                ahci.queue_wait_hist[bucket]);
    writer->add(writer->context, PERF_COUNTER_AHCI_SERVICE_HIST_BASE + bucket,
                ahci.service_hist[bucket]);
  }
}

void kernel_perf_counter_init(void) {
  perf_counter_register_collector(collect_external_counters);
}

#else

void kernel_perf_counter_init(void) {}

#endif
