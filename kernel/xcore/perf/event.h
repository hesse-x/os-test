/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_KERNEL_PERF_EVENT_H
#define XOS_KERNEL_PERF_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef PERF
void perf_trace_causal(uint8_t type, uint8_t stage, uint32_t cookie);
uint32_t perf_trace_next_cookie(void);
void perf_trace_sched_switch(int32_t prev_pid, int32_t next_pid,
                             uint8_t prev_state, uint8_t prev_wait,
                             bool prev_idle, bool next_idle);
void perf_trace_task_wake(int32_t pid, uint8_t wait_event);
void perf_trace_irq(uint8_t type, uint16_t vector, int32_t owner);
uint32_t perf_event_capture(unsigned bank, uint32_t first_sequence);
const uint8_t *perf_event_data(unsigned bank);
uint64_t perf_event_lost(void);
#else
static inline void perf_trace_causal(uint8_t type, uint8_t stage,
                                     uint32_t cookie) {
  (void)type;
  (void)stage;
  (void)cookie;
}
static inline uint32_t perf_trace_next_cookie(void) { return 0; }
static inline void perf_trace_sched_switch(int32_t prev_pid, int32_t next_pid,
                                           uint8_t prev_state,
                                           uint8_t prev_wait, bool prev_idle,
                                           bool next_idle) {
  (void)prev_pid;
  (void)next_pid;
  (void)prev_state;
  (void)prev_wait;
  (void)prev_idle;
  (void)next_idle;
}
static inline void perf_trace_task_wake(int32_t pid, uint8_t wait_event) {
  (void)pid;
  (void)wait_event;
}
static inline void perf_trace_irq(uint8_t type, uint16_t vector,
                                  int32_t owner) {
  (void)type;
  (void)vector;
  (void)owner;
}
#endif

#endif
