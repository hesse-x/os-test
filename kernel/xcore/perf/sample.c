/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/perf/sample.h"

#ifdef PERF

#include <stdbool.h>

#include <xos/perf.h>

#include "arch/x64/smp.h"
#include "arch/x64/utils.h"

#define PERF_CHAINS_PER_CPU 256U
#define PERF_CHAIN_RECORDS_MAX                                                 \
  (MAX_CPUS * PERF_CHAINS_PER_CPU * (PERF_CALLCHAIN_MAX_DEPTH + 1U))

struct perf_chain_slot {
  uint64_t hash;
  uint64_t frames[PERF_CALLCHAIN_MAX_DEPTH];
  uint32_t count;
  uint8_t depth;
  uint8_t source;
  uint8_t unwind_stop;
  uint8_t reserved;
};

struct perf_sample_record {
  uint64_t payload;
  uint32_t sequence;
  uint16_t ident;
  uint8_t kind;
  uint8_t aux;
  uint32_t committed_size;
  uint32_t value;
};

_Static_assert(sizeof(struct perf_sample_record) == XOS_PERF_EARLY_RECORD_SIZE,
               "sample record ABI size");

static struct perf_chain_slot chain_slots[MAX_CPUS][PERF_CHAINS_PER_CPU];
static struct perf_sample_record sample_snapshots[2][PERF_CHAIN_RECORDS_MAX];
static uint64_t sample_snapshot_hits[2];
static uint64_t sample_lost_count;
static uint64_t sample_truncated_count;
static uint32_t sample_active_writers[MAX_CPUS];
static bool sample_accepting = true;

static uint64_t chain_hash(const uint64_t *frames, uint8_t depth,
                           uint8_t source) {
  uint64_t hash = 1469598103934665603ULL ^ source;
  for (uint8_t i = 0; i < depth; i++) {
    hash ^= frames[i];
    hash *= 1099511628211ULL;
  }
  return hash ? hash : 1;
}

void perf_record_callchain(unsigned cpu, const uint64_t *frames, uint8_t depth,
                           uint8_t source, uint8_t unwind_stop) {
  if (cpu >= MAX_CPUS || depth == 0)
    return;
  if (!__atomic_load_n(&sample_accepting, __ATOMIC_ACQUIRE))
    return;
  __atomic_fetch_add(&sample_active_writers[cpu], 1U, __ATOMIC_ACQUIRE);
  if (!__atomic_load_n(&sample_accepting, __ATOMIC_ACQUIRE)) {
    __atomic_fetch_sub(&sample_active_writers[cpu], 1U, __ATOMIC_RELEASE);
    return;
  }
  if (depth > PERF_CALLCHAIN_MAX_DEPTH)
    depth = PERF_CALLCHAIN_MAX_DEPTH;
  uint64_t hash = chain_hash(frames, depth, source);
  uint32_t start = (uint32_t)hash & (PERF_CHAINS_PER_CPU - 1U);
  for (uint32_t probe = 0; probe < PERF_CHAINS_PER_CPU; probe++) {
    struct perf_chain_slot *slot =
        &chain_slots[cpu][(start + probe) & (PERF_CHAINS_PER_CPU - 1U)];
    uint64_t existing = __atomic_load_n(&slot->hash, __ATOMIC_ACQUIRE);
    if (existing == hash && slot->depth == depth && slot->source == source) {
      bool equal = true;
      for (uint8_t i = 0; i < depth; i++)
        equal = equal && slot->frames[i] == frames[i];
      if (equal) {
        __atomic_fetch_add(&slot->count, 1U, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&sample_active_writers[cpu], 1U, __ATOMIC_RELEASE);
        return;
      }
    }
    if (existing != 0)
      continue;
    for (uint8_t i = 0; i < depth; i++)
      slot->frames[i] = frames[i];
    slot->depth = depth;
    slot->source = source;
    slot->unwind_stop = unwind_stop;
    slot->count = 1;
    __atomic_store_n(&slot->hash, hash, __ATOMIC_RELEASE);
    if (unwind_stop != XOS_PERF_UNWIND_COMPLETE)
      __atomic_fetch_add(&sample_truncated_count, 1U, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&sample_active_writers[cpu], 1U, __ATOMIC_RELEASE);
    return;
  }
  __atomic_fetch_add(&sample_lost_count, 1U, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&sample_active_writers[cpu], 1U, __ATOMIC_RELEASE);
}

static void write_record(struct perf_sample_record *record, uint64_t payload,
                         uint32_t sequence, uint16_t ident, uint8_t kind,
                         uint8_t aux, uint32_t value) {
  record->payload = payload;
  record->sequence = sequence;
  record->ident = ident;
  record->kind = kind;
  record->aux = aux;
  record->committed_size = sizeof(*record);
  record->value = value;
}

uint32_t perf_sample_capture(unsigned bank, uint32_t first_sequence) {
  bank &= 1U;
  uint32_t out = 0;
  uint64_t hits = 0;
  for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++) {
    for (uint32_t i = 0; i < PERF_CHAINS_PER_CPU; i++) {
      struct perf_chain_slot *slot = &chain_slots[cpu][i];
      if (__atomic_load_n(&slot->hash, __ATOMIC_ACQUIRE) == 0)
        continue;
      uint32_t count = __atomic_load_n(&slot->count, __ATOMIC_ACQUIRE);
      uint8_t depth = slot->depth;
      if (count == 0 || depth == 0 || depth > PERF_CALLCHAIN_MAX_DEPTH)
        continue;
      write_record(&sample_snapshots[bank][out], slot->hash,
                   first_sequence + out, (uint16_t)((cpu << 8) | depth),
                   XOS_PERF_CALLCHAIN, slot->source, count);
      out++;
      for (uint8_t frame = 0; frame < depth; frame++) {
        uint8_t stop = frame + 1U == depth ? slot->unwind_stop : 0;
        write_record(&sample_snapshots[bank][out], slot->frames[frame],
                     first_sequence + out, frame, XOS_PERF_CALLCHAIN_FRAME,
                     stop, 0);
        out++;
      }
      hits += count;
    }
  }
  sample_snapshot_hits[bank] = hits;
  return out * sizeof(struct perf_sample_record);
}

const uint8_t *perf_sample_data(unsigned bank) {
  return (const uint8_t *)sample_snapshots[bank & 1U];
}

uint64_t perf_sample_lost(void) {
  return __atomic_load_n(&sample_lost_count, __ATOMIC_RELAXED);
}

uint64_t perf_sample_hits(unsigned bank) {
  return sample_snapshot_hits[bank & 1U];
}

uint64_t perf_sample_truncated(void) {
  return __atomic_load_n(&sample_truncated_count, __ATOMIC_RELAXED);
}

void perf_sample_reset(void) {
  __atomic_store_n(&sample_accepting, false, __ATOMIC_RELEASE);
  for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++)
    while (__atomic_load_n(&sample_active_writers[cpu], __ATOMIC_ACQUIRE))
      __asm__ volatile("pause");

  __memset(chain_slots, 0, sizeof(chain_slots));
  sample_lost_count = 0;
  sample_truncated_count = 0;
  __atomic_store_n(&sample_accepting, true, __ATOMIC_RELEASE);
}

void perf_sample_stop(void) {
  __atomic_store_n(&sample_accepting, false, __ATOMIC_RELEASE);
  for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++)
    while (__atomic_load_n(&sample_active_writers[cpu], __ATOMIC_ACQUIRE))
      __asm__ volatile("pause");
}

#endif
