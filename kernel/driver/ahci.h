/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_AHCI_H
#define KERNEL_AHCI_H

#include <stdint.h>

#include "kernel/driver/driver.h"

#define AHCI_MAX_SECTORS 128 // 64KB bounce buffer / 512 bytes per sector

#define AHCI_TIMING_BUCKETS 32
#define AHCI_IRQ_STAGE_COUNT 8
enum ahci_irq_stage {
  AHCI_IRQ_ACK = 0,
  AHCI_IRQ_LOCK_WAIT,
  AHCI_IRQ_BOOKKEEPING,
  AHCI_IRQ_COPY,
  AHCI_IRQ_WAKE,
  AHCI_IRQ_NEXT_SUBMIT,
  AHCI_IRQ_UNLOCK_EXIT,
  AHCI_IRQ_LOCKED_TOTAL,
};

struct ahci_stage_stats {
  uint64_t count;
  uint64_t cycles;
  uint64_t max;
  uint64_t hist[AHCI_TIMING_BUCKETS];
};

struct ahci_stats {
  uint64_t sync_submitted;
  uint64_t async_submitted;
  uint64_t completed;
  uint64_t errors;
  uint64_t sync_wakes;
  uint64_t async_wakes;
  uint64_t early_completes;
  uint64_t cross_cpu_wakes;
  uint64_t queue_full;
  uint64_t invalid_timing;
  uint64_t queue_wait_count;
  uint64_t queue_wait_cycles;
  uint64_t queue_wait_max;
  uint64_t service_count;
  uint64_t service_cycles;
  uint64_t service_max;
  uint64_t queue_depth_cycles;
  uint64_t queue_depth_last_tsc;
  uint32_t queue_depth;
  uint32_t queue_depth_max;
  uint64_t queue_wait_hist[AHCI_TIMING_BUCKETS];
  uint64_t service_hist[AHCI_TIMING_BUCKETS];
  struct ahci_stage_stats irq_stage[AHCI_IRQ_STAGE_COUNT];
  uint64_t irq_handler_count;
  uint64_t irq_handler_cycles;
  uint64_t irq_handler_max;
  uint64_t irq_handler_hist[AHCI_TIMING_BUCKETS];
  uint64_t spurious_count;
  uint64_t spurious_cycles;
  uint64_t orphan_count;
  uint64_t orphan_cycles;
  uint64_t long_tail_count[2];
  uint64_t long_tail_stage_cycles[2][7];
  uint64_t long_tail_stage_max[2][7];
};

void ahci_init();
int ahci_submit_sync(uint32_t lba, uint32_t count, void *buf, uint8_t dir);
int ahci_flush_cache(void);
uint64_t ahci_sector_count(void);
void ahci_get_stats(struct ahci_stats *out);

// Submit async block request. Returns cookie (>0) on success, -errno on error.
// Completion delivered via RECV_NOTIFY to caller.
int ahci_submit_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir);

// Switch active port when no request is in flight (re-initializes the port).
// Returns 0 on success, -errno if port has no device.
int ahci_set_active_port(int port);

extern struct dev_driver ahci_driver;

#endif // KERNEL_AHCI_H
