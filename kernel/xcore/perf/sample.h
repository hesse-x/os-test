/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_KERNEL_PERF_SAMPLE_H
#define XOS_KERNEL_PERF_SAMPLE_H

#include <stdint.h>

#define PERF_CALLCHAIN_MAX_DEPTH 32U

#ifdef PERF
void perf_record_callchain(unsigned cpu, const uint64_t *frames, uint8_t depth,
                           uint8_t source, uint8_t unwind_stop);
uint32_t perf_sample_capture(unsigned bank, uint32_t first_sequence);
const uint8_t *perf_sample_data(unsigned bank);
uint64_t perf_sample_lost(void);
uint64_t perf_sample_hits(unsigned bank);
uint64_t perf_sample_truncated(void);
void perf_sample_stop(void);
#else
static inline void perf_record_callchain(unsigned cpu, const uint64_t *frames,
                                         uint8_t depth, uint8_t source,
                                         uint8_t unwind_stop) {
  (void)cpu;
  (void)frames;
  (void)depth;
  (void)source;
  (void)unwind_stop;
}
#endif

#endif
