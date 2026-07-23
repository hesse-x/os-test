/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_BSD_FILE_FAULT_H
#define KERNEL_BSD_FILE_FAULT_H

#include "kernel/xcore/xtask.h"
#include <stdint.h>

// S12: file-backed mmap page-in. Registered as the Xcore fault_handler hook
// (kernel/xcore/trap.c #PF path). Called for a not-present (or write-protect)
// page fault after the fork-COW branch has had its chance. Returns 1 if the
// fault was serviced (retry the instruction), 0 if this vaddr is not a
// file-backed mapping and the caller should fall through to SIGSEGV.
int file_fault_handler(uint64_t fault_addr, xtask *t);

#endif // KERNEL_BSD_FILE_FAULT_H
