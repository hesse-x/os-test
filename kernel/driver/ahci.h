/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_AHCI_H
#define KERNEL_AHCI_H

#include "kernel/driver/driver.h"
#include "kernel/xcore/spinlock.h"
#include <stdbool.h>
#include <stdint.h>

#define AHCI_MAX_SECTORS 128 // 64KB bounce buffer / 512 bytes per sector

void ahci_init();
int ahci_read_lba(uint32_t lba, uint32_t count, void *buf);
int ahci_write_lba(uint32_t lba, uint32_t count, const void *buf);

// Submit async block request. Returns cookie (>0) on success, -errno on error.
// Completion delivered via RECV_NOTIFY to caller.
int ahci_submit_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir);

// Check if AHCI has an async request in flight (for EBUSY safety check)
bool ahci_is_busy();

// Switch active port for polling I/O (re-initializes the port).
// Returns 0 on success, -errno if port has no device.
int ahci_set_active_port(int port);

extern spinlock ahci_lock;

extern struct dev_driver ahci_driver;

#endif // KERNEL_AHCI_H
