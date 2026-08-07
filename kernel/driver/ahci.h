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

void ahci_init();
int ahci_submit_sync(uint32_t lba, uint32_t count, void *buf, uint8_t dir);
uint64_t ahci_sector_count(void);

// Submit async block request. Returns cookie (>0) on success, -errno on error.
// Completion delivered via RECV_NOTIFY to caller.
int ahci_submit_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir);

// Switch active port when no request is in flight (re-initializes the port).
// Returns 0 on success, -errno if port has no device.
int ahci_set_active_port(int port);

extern struct dev_driver ahci_driver;

#endif // KERNEL_AHCI_H
