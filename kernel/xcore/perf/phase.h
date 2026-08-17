/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_KERNEL_PERF_PHASE_H
#define XOS_KERNEL_PERF_PHASE_H

#ifdef PERF
#include <stdint.h>

void perf_early_phase_begin_at(uint16_t phase_id, uint64_t timestamp);
void perf_phase_begin(uint16_t phase_id);
void perf_phase_end(uint16_t phase_id);
void perf_mark(uint16_t mark_id, uint8_t stage, uint32_t value);
uint32_t perf_early_committed_bytes(void);
uint32_t perf_early_record_count(void);
const uint8_t *perf_early_data(void);

#define PERF_PHASE_BEGIN(id) perf_phase_begin((id))
#define PERF_PHASE_END(id) perf_phase_end((id))
#else
#define PERF_PHASE_BEGIN(id)                                                   \
  do {                                                                         \
  } while (0)
#define PERF_PHASE_END(id)                                                     \
  do {                                                                         \
  } while (0)
#endif

#endif
