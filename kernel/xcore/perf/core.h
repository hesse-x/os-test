/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_KERNEL_PERF_CORE_H
#define XOS_KERNEL_PERF_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct xtask;

#ifdef PERF
void perf_register_target(struct xtask *task);
void perf_target_exit(struct xtask *task, int32_t status);
int perf_freeze(uint32_t reason, bool complete);
int perf_checkpoint(void);
void perf_get_info(void *info);
void perf_get_metadata(void *metadata);
size_t perf_raw_read(uint64_t offset, void *buffer, size_t length);
void perf_watchdog_tick(void);
#else
static inline void perf_watchdog_tick(void) {}
static inline void perf_target_exit(struct xtask *task, int32_t status) {
  (void)task;
  (void)status;
}
#endif

#endif
