/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/perf/event.h"

#ifdef PERF

#include <stdbool.h>

#include <xos/perf.h>

#include "arch/x64/smp.h"
#include "arch/x64/utils.h"

#define PERF_EVENT_MAX 2400000U
#define PERF_IRQ_VECTOR_COUNT 256U
#define PERF_IRQ_SUMMARY_MAX (MAX_CPUS * PERF_IRQ_VECTOR_COUNT * 3U)

struct perf_event_record {
  uint64_t timestamp;
  uint32_t sequence;
  uint16_t ident;
  uint8_t kind;
  uint8_t cpu;
  uint32_t committed_size;
  uint32_t value;
};

_Static_assert(sizeof(struct perf_event_record) == XOS_PERF_EARLY_RECORD_SIZE,
               "trace event ABI size");
_Static_assert(PERF_EVENT_MAX <= UINT32_MAX / XOS_PERF_EARLY_RECORD_SIZE,
               "event capture byte size fits uint32_t");
_Static_assert((uint64_t)(PERF_EVENT_MAX + PERF_IRQ_SUMMARY_MAX) *
                       XOS_PERF_EARLY_RECORD_SIZE <
                   128ULL * 1024ULL * 1024ULL,
               "event capture fits raw file limit");

static struct perf_event_record event_slots[PERF_EVENT_MAX];
static struct perf_event_record
    event_snapshots[2][PERF_EVENT_MAX + PERF_IRQ_SUMMARY_MAX];
static uint32_t event_head;
static uint32_t event_attempted[MAX_CPUS];
static uint32_t event_committed[MAX_CPUS];
static uint32_t event_high_water[MAX_CPUS];
static uint32_t event_active_writers[MAX_CPUS];
static uint64_t event_lost_count;
static uint32_t causal_cookie;
static bool event_accepting = true;

struct perf_irq_stat {
  uint64_t start;
  uint64_t count;
  uint64_t total_cycles;
  uint64_t max_cycles;
  int32_t owner;
};

static struct perf_irq_stat irq_stats[MAX_CPUS][PERF_IRQ_VECTOR_COUNT];
/* Zero means no traced IRQ; otherwise vector + 1. */
static uint16_t active_irq[MAX_CPUS];

uint32_t perf_trace_next_cookie(void) {
  uint32_t cookie = __atomic_add_fetch(&causal_cookie, 1U, __ATOMIC_RELAXED);
  return cookie ? cookie
                : __atomic_add_fetch(&causal_cookie, 1U, __ATOMIC_RELAXED);
}

static void emit_timed(uint8_t type, uint8_t subtype, uint32_t value) {
  unsigned cpu = (unsigned)get_cpu_local()->cpu_id;
  if (cpu >= MAX_CPUS || !__atomic_load_n(&event_accepting, __ATOMIC_ACQUIRE))
    return;
  __atomic_fetch_add(&event_active_writers[cpu], 1U, __ATOMIC_ACQUIRE);
  if (!__atomic_load_n(&event_accepting, __ATOMIC_ACQUIRE)) {
    __atomic_fetch_sub(&event_active_writers[cpu], 1U, __ATOMIC_RELEASE);
    return;
  }
  uint32_t attempted =
      __atomic_fetch_add(&event_attempted[cpu], 1U, __ATOMIC_RELAXED) + 1U;
  uint32_t slot = __atomic_fetch_add(&event_head, 1U, __ATOMIC_RELAXED);
  if (slot >= PERF_EVENT_MAX) {
    __atomic_fetch_add(&event_lost_count, 1U, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&event_active_writers[cpu], 1U, __ATOMIC_RELEASE);
    return;
  }
  uint32_t high = attempted;
  uint32_t old = __atomic_load_n(&event_high_water[cpu], __ATOMIC_RELAXED);
  while (high > old &&
         !__atomic_compare_exchange_n(&event_high_water[cpu], &old, high, true,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
  struct perf_event_record *record = &event_slots[slot];
  record->timestamp = rdtsc64();
  record->sequence = 0;
  record->ident = (uint16_t)type | ((uint16_t)subtype << 8);
  record->kind = XOS_PERF_TRACE_EVENT;
  record->cpu = (uint8_t)cpu;
  record->value = value;
  __atomic_store_n(&record->committed_size, sizeof(*record), __ATOMIC_RELEASE);
  __atomic_fetch_add(&event_committed[cpu], 1U, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&event_active_writers[cpu], 1U, __ATOMIC_RELEASE);
}

void perf_trace_causal(uint8_t type, uint8_t stage, uint32_t cookie) {
  if (cookie == 0)
    return;
  if (type == XOS_PERF_TRACE_IO && stage == XOS_PERF_IO_COMPLETE) {
    unsigned cpu = (unsigned)get_cpu_local()->cpu_id;
    uint16_t active = cpu < MAX_CPUS
                          ? __atomic_load_n(&active_irq[cpu], __ATOMIC_ACQUIRE)
                          : 0;
    if (active != 0)
      emit_timed(XOS_PERF_TRACE_IRQ_CAUSE, (uint8_t)(active - 1U), cookie);
  }
  emit_timed(type, stage, cookie);
}

void perf_trace_sched_switch(int32_t prev_pid, int32_t next_pid,
                             uint8_t prev_state, uint8_t prev_wait,
                             bool prev_idle, bool next_idle) {
  uint32_t packed = ((uint32_t)(uint16_t)prev_pid << 16) | (uint16_t)next_pid;
  uint8_t flags =
      prev_state | (prev_idle ? 1U << 3 : 0) | (next_idle ? 1U << 4 : 0);
  emit_timed(XOS_PERF_TRACE_SCHED_SWITCH, flags, packed);
  if (prev_state == 3)
    emit_timed(XOS_PERF_TRACE_TASK_BLOCK, prev_wait, (uint16_t)prev_pid);
}

void perf_trace_task_wake(int32_t pid, uint8_t wait_event) {
  emit_timed(XOS_PERF_TRACE_TASK_WAKE, wait_event, (uint16_t)pid);
}

void perf_trace_exec(int32_t pid, uint8_t kind) {
  if (kind != 0)
    emit_timed(XOS_PERF_TRACE_EXEC, kind, (uint32_t)pid);
}

void perf_trace_irq(uint8_t type, uint16_t vector, int32_t owner) {
  unsigned cpu = (unsigned)get_cpu_local()->cpu_id;
  if (cpu >= MAX_CPUS || vector >= PERF_IRQ_VECTOR_COUNT)
    return;
  struct perf_irq_stat *stat = &irq_stats[cpu][vector];
  if (type == XOS_PERF_TRACE_IRQ_BEGIN) {
    __atomic_store_n(&stat->owner, owner, __ATOMIC_RELAXED);
    __atomic_store_n(&active_irq[cpu], (uint16_t)(vector + 1U),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&stat->start, rdtsc64(), __ATOMIC_RELEASE);
    return;
  }
  if (type != XOS_PERF_TRACE_IRQ_END)
    return;
  uint64_t end = rdtsc64();
  uint64_t start = __atomic_exchange_n(&stat->start, 0, __ATOMIC_ACQ_REL);
  __atomic_store_n(&active_irq[cpu], 0, __ATOMIC_RELEASE);
  if (start == 0 || end < start)
    return;
  uint64_t duration = end - start;
  __atomic_fetch_add(&stat->count, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&stat->total_cycles, duration, __ATOMIC_RELAXED);
  uint64_t old_max = __atomic_load_n(&stat->max_cycles, __ATOMIC_RELAXED);
  while (duration > old_max && !__atomic_compare_exchange_n(
                                   &stat->max_cycles, &old_max, duration, true,
                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

static void write_irq_summary(struct perf_event_record *record,
                              uint64_t payload, uint32_t sequence, uint8_t type,
                              uint8_t vector, uint8_t cpu, int32_t owner) {
  record->timestamp = payload;
  record->sequence = sequence;
  record->ident = (uint16_t)type | ((uint16_t)vector << 8);
  record->kind = XOS_PERF_TRACE_EVENT;
  record->cpu = cpu;
  record->value = (uint32_t)owner;
  record->committed_size = sizeof(*record);
}

uint32_t perf_event_capture(unsigned bank, uint32_t first_sequence,
                            bool *valid) {
  bank &= 1U;
  if (valid)
    *valid = true;
  uint32_t out = 0;
  uint32_t limit = __atomic_load_n(&event_head, __ATOMIC_ACQUIRE);
  if (limit > PERF_EVENT_MAX)
    limit = PERF_EVENT_MAX;
  for (uint32_t i = 0; i < limit; i++) {
    struct perf_event_record *source = &event_slots[i];
    if (__atomic_load_n(&source->committed_size, __ATOMIC_ACQUIRE) !=
        sizeof(*source)) {
      if (valid)
        *valid = false;
      continue;
    }
    event_snapshots[bank][out] = *source;
    event_snapshots[bank][out].sequence = first_sequence + out;
    out++;
  }
  for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++) {
    for (unsigned vector = 0; vector < PERF_IRQ_VECTOR_COUNT; vector++) {
      struct perf_irq_stat *stat = &irq_stats[cpu][vector];
      uint64_t count = __atomic_load_n(&stat->count, __ATOMIC_ACQUIRE);
      if (count == 0)
        continue;
      int32_t owner = __atomic_load_n(&stat->owner, __ATOMIC_RELAXED);
      write_irq_summary(&event_snapshots[bank][out], count,
                        first_sequence + out, XOS_PERF_TRACE_IRQ_COUNT,
                        (uint8_t)vector, (uint8_t)cpu, owner);
      out++;
      write_irq_summary(&event_snapshots[bank][out],
                        __atomic_load_n(&stat->total_cycles, __ATOMIC_ACQUIRE),
                        first_sequence + out, XOS_PERF_TRACE_IRQ_TOTAL,
                        (uint8_t)vector, (uint8_t)cpu, owner);
      out++;
      write_irq_summary(&event_snapshots[bank][out],
                        __atomic_load_n(&stat->max_cycles, __ATOMIC_ACQUIRE),
                        first_sequence + out, XOS_PERF_TRACE_IRQ_MAX,
                        (uint8_t)vector, (uint8_t)cpu, owner);
      out++;
    }
  }
  return out * sizeof(struct perf_event_record);
}

const uint8_t *perf_event_data(unsigned bank) {
  return (const uint8_t *)event_snapshots[bank & 1U];
}

uint64_t perf_event_lost(void) {
  return __atomic_load_n(&event_lost_count, __ATOMIC_RELAXED);
}

void perf_event_reset(void) {
  __atomic_store_n(&event_accepting, false, __ATOMIC_RELEASE);
  for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++)
    while (__atomic_load_n(&event_active_writers[cpu], __ATOMIC_ACQUIRE))
      __asm__ volatile("pause");

  __memset(event_slots, 0, sizeof(event_slots));
  event_head = 0;
  __memset(event_attempted, 0, sizeof(event_attempted));
  __memset(event_committed, 0, sizeof(event_committed));
  __memset(event_high_water, 0, sizeof(event_high_water));
  event_lost_count = 0;
  __atomic_store_n(&event_accepting, true, __ATOMIC_RELEASE);
}

void perf_event_stop(void) {
  __atomic_store_n(&event_accepting, false, __ATOMIC_RELEASE);
  for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++)
    while (__atomic_load_n(&event_active_writers[cpu], __ATOMIC_ACQUIRE))
      __asm__ volatile("pause");
}

unsigned perf_event_cpu_count(void) { return MAX_CPUS; }

void perf_event_get_cpu_stats(unsigned cpu, struct perf_event_cpu_stats *out) {
  if (!out)
    return;
  *out = (struct perf_event_cpu_stats){0};
  if (cpu >= MAX_CPUS)
    return;
  out->capacity = PERF_EVENT_MAX;
  out->attempted = __atomic_load_n(&event_attempted[cpu], __ATOMIC_RELAXED);
  out->committed = __atomic_load_n(&event_committed[cpu], __ATOMIC_RELAXED);
  out->high_water = __atomic_load_n(&event_high_water[cpu], __ATOMIC_RELAXED);
}

#endif
