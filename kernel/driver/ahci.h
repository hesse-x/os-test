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

#define AHCI_TIMING_BUCKETS 16
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
};

void ahci_init();
int ahci_submit_sync(uint32_t lba, uint32_t count, void *buf, uint8_t dir);
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
