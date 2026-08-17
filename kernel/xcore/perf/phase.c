/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/perf/phase.h"

#ifdef PERF

#include <stdbool.h>
#include <stdint.h>

#include <xos/perf.h>

#include "kernel/xcore/perf/phase_ids.h"

#define PERF_EARLY_BUFFER_SIZE 65536U
#define PERF_EARLY_STACK_DEPTH 32U

enum perf_early_kind {
  PERF_EARLY_PHASE_BEGIN = 1,
  PERF_EARLY_PHASE_END = 2,
  PERF_EARLY_TRACE_ERROR = 3,
};

typedef struct perf_early_record {
  uint64_t timestamp;
  uint32_t sequence;
  uint16_t phase_id;
  uint8_t kind;
  uint8_t reserved;
  uint32_t committed_size;
  uint32_t reserved2;
} perf_early_record;

_Static_assert(sizeof(perf_early_record) == 24, "early record ABI size");

extern uint8_t perf_early_buffer[] __attribute__((visibility("hidden")));
extern uint8_t perf_early_buffer_end[] __attribute__((visibility("hidden")));

static uint32_t early_offset;
static uint32_t early_sequence;
static uint16_t early_stack[PERF_EARLY_STACK_DEPTH];
static uint8_t early_depth;

static inline uint64_t perf_rdtsc(void) {
  uint32_t low;
  uint32_t high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return ((uint64_t)high << 32) | low;
}

static bool perf_phase_known(uint16_t id) {
  // Duplicate IDs deliberately fail compilation as duplicate case labels.
  switch (id) {
  case PERF_PHASE_BOOT_TO_KERNEL_MAIN:
  case PERF_PHASE_EARLY_PAGING:
  case PERF_PHASE_EARLY_GDT:
  case PERF_PHASE_EARLY_HIGHER_HALF:
  case PERF_PHASE_XCORE:
  case PERF_PHASE_MEMORY:
  case PERF_PHASE_ACPI:
  case PERF_PHASE_IDT:
  case PERF_PHASE_APIC_TSC:
  case PERF_PHASE_SCHEDULER:
  case PERF_PHASE_SMP:
  case PERF_PHASE_VFS_CORE:
  case PERF_PHASE_INODE:
  case PERF_PHASE_PAGE_CACHE:
  case PERF_PHASE_DEVTMPFS:
  case PERF_PHASE_DRIVER:
  case PERF_PHASE_PCI:
  case PERF_PHASE_AHCI:
  case PERF_PHASE_XHCI:
  case PERF_PHASE_DRM:
  case PERF_PHASE_BSD:
  case PERF_PHASE_INIT_ELF:
  case PERF_PHASE_INIT:
  case PERF_PHASE_SERVICE_SYSLOGD:
  case PERF_PHASE_SERVICE_EVDEV:
  case PERF_PHASE_SERVICE_UDEVD:
  case PERF_PHASE_SERVICE_SEATD:
  case PERF_PHASE_SERVICE_COMPOSITOR:
  case PERF_PHASE_SERVICE_DESKTOP:
  case PERF_PHASE_SERVICE_SHELL:
  case PERF_PHASE_TEST_RUNNER:
  case PERF_PHASE_TEST_CASE:
    return true;
  default:
    return false;
  }
}

static void perf_early_emit(uint16_t phase_id, uint8_t kind,
                            uint64_t timestamp) {
  uint32_t offset = __atomic_load_n(&early_offset, __ATOMIC_RELAXED);
  if (offset > PERF_EARLY_BUFFER_SIZE - sizeof(perf_early_record))
    return;

  perf_early_record *record = (perf_early_record *)(perf_early_buffer + offset);
  record->timestamp = timestamp;
  record->sequence = early_sequence++;
  record->phase_id = phase_id;
  record->kind = kind;
  record->reserved = 0;
  record->reserved2 = 0;
  __atomic_store_n(&record->committed_size, sizeof(*record), __ATOMIC_RELEASE);
  __atomic_store_n(&early_offset, offset + sizeof(*record), __ATOMIC_RELEASE);
}

void perf_early_phase_begin_at(uint16_t phase_id, uint64_t timestamp) {
  if (!perf_phase_known(phase_id) || early_depth >= PERF_EARLY_STACK_DEPTH) {
    perf_early_emit(phase_id, PERF_EARLY_TRACE_ERROR, timestamp);
    return;
  }
  early_stack[early_depth++] = phase_id;
  perf_early_emit(phase_id, PERF_EARLY_PHASE_BEGIN, timestamp);
}

void perf_phase_begin(uint16_t phase_id) {
  perf_early_phase_begin_at(phase_id, perf_rdtsc());
}

void perf_phase_end(uint16_t phase_id) {
  uint64_t timestamp = perf_rdtsc();
  if (!perf_phase_known(phase_id) || early_depth == 0 ||
      early_stack[early_depth - 1] != phase_id) {
    perf_early_emit(phase_id, PERF_EARLY_TRACE_ERROR, timestamp);
    return;
  }
  early_depth--;
  perf_early_emit(phase_id, PERF_EARLY_PHASE_END, timestamp);
}

void perf_mark(uint16_t mark_id, uint8_t stage, uint32_t value) {
  uint64_t timestamp = perf_rdtsc();
  if (mark_id == 0 ||
      (stage != XOS_PERF_MARK_BEGIN && stage != XOS_PERF_MARK_END)) {
    perf_early_emit(mark_id, PERF_EARLY_TRACE_ERROR, timestamp);
    return;
  }
  uint32_t offset = __atomic_load_n(&early_offset, __ATOMIC_RELAXED);
  if (offset > PERF_EARLY_BUFFER_SIZE - sizeof(perf_early_record))
    return;

  perf_early_record *record = (perf_early_record *)(perf_early_buffer + offset);
  record->timestamp = timestamp;
  record->sequence = early_sequence++;
  record->phase_id = mark_id;
  record->kind = XOS_PERF_MARK_EVENT;
  record->reserved = stage;
  record->reserved2 = value;
  __atomic_store_n(&record->committed_size, sizeof(*record), __ATOMIC_RELEASE);
  __atomic_store_n(&early_offset, offset + sizeof(*record), __ATOMIC_RELEASE);
}

uint32_t perf_early_committed_bytes(void) {
  return __atomic_load_n(&early_offset, __ATOMIC_ACQUIRE);
}

uint32_t perf_early_record_count(void) {
  return early_offset / sizeof(perf_early_record);
}

const uint8_t *perf_early_data(void) { return perf_early_buffer; }

#endif
