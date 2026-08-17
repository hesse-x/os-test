/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/perf/core.h"

#ifdef PERF

#include <xos/perf.h>

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/perf/counter.h"
#include "kernel/xcore/perf/event.h"
#include "kernel/xcore/perf/phase.h"
#include "kernel/xcore/perf/pmu.h"
#include "kernel/xcore/perf/sample.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/xtask.h"

#define PERF_HEADER_SIZE XOS_PERF_FILE_HEADER_SIZE
#define PERF_FOOTER_SIZE XOS_PERF_FILE_FOOTER_SIZE

extern uint64_t perf_boot_tsc __attribute__((visibility("hidden")));
extern uint8_t __perf_build_id_note_start[]
    __attribute__((visibility("hidden")));
extern uint8_t __perf_build_id_note_end[] __attribute__((visibility("hidden")));

static uint32_t perf_state = XOS_PERF_RUNNING;
static xtask *perf_target;
static spinlock perf_snapshot_lock = SPINLOCK_INIT;

struct perf_snapshot {
  uint64_t end_timestamp;
  uint32_t early_size;
  uint32_t event_size;
  uint32_t sample_size;
  uint32_t counter_size;
  uint32_t end_reason;
  uint32_t sample_bank;
  uint32_t event_bank;
  uint32_t counter_bank;
  uint64_t lost_samples;
  uint64_t sample_hits;
  bool complete;
  bool available;
};

static struct perf_snapshot perf_snapshot;
static uint8_t perf_watchdog_requested;

static uint64_t snapshot_records_size(const struct perf_snapshot *snapshot) {
  return (uint64_t)snapshot->early_size + snapshot->event_size +
         snapshot->counter_size + snapshot->sample_size;
}

static void put16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(value >> (i * 8));
}

static void put64(uint8_t *p, uint64_t value) {
  for (unsigned i = 0; i < 8; i++)
    p[i] = (uint8_t)(value >> (i * 8));
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
  crc = ~crc;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

static void build_header(uint8_t header[PERF_HEADER_SIZE],
                         const struct perf_snapshot *snapshot) {
  __memset(header, 0, PERF_HEADER_SIZE);
  __memcpy(header, "XOSPERF\0", 8);
  put16(header + 8, XOS_PERF_RAW_MAJOR);
  put16(header + 10, XOS_PERF_RAW_MINOR);
  header[12] = 1;
  header[13] = 64;
  put16(header + 14, PERF_HEADER_SIZE);
  put32(header + 16, snapshot->complete ? 1U : 0U);
  put32(header + 20, 1);
  // GNU SHA-1 note header is 16 bytes; store the first 16 descriptor bytes.
  if (__perf_build_id_note_end - __perf_build_id_note_start >= 36)
    __memcpy(header + 24, __perf_build_id_note_start + 16, 16);
  put64(header + 40, perf_boot_tsc);
  put64(header + 48, tsc_freq);
  put64(header + 56, perf_boot_tsc);
  put64(header + 64, snapshot->end_timestamp);
  put64(header + 72, snapshot_records_size(snapshot));
  put32(header + 80, crc32_update(0, header, PERF_HEADER_SIZE));
}

static void build_footer(uint8_t footer[PERF_FOOTER_SIZE],
                         const struct perf_snapshot *snapshot) {
  __memset(footer, 0, PERF_FOOTER_SIZE);
  __memcpy(footer, "XOSEND\0\0", 8);
  uint64_t records_size = snapshot_records_size(snapshot);
  put64(footer + 8, records_size);
  put64(footer + 16, records_size / XOS_PERF_EARLY_RECORD_SIZE);
  put32(footer + 24, snapshot->end_reason);
  put32(footer + 28, snapshot->complete ? 1U : 0U);
  put64(footer + 32, snapshot->lost_samples);
  put64(footer + 40, snapshot->sample_hits);
  uint32_t crc = crc32_update(0, perf_early_data(), snapshot->early_size);
  crc = crc32_update(crc, perf_event_data(snapshot->event_bank),
                     snapshot->event_size);
  crc = crc32_update(crc, perf_counter_data(snapshot->counter_bank),
                     snapshot->counter_size);
  crc = crc32_update(crc, perf_sample_data(snapshot->sample_bank),
                     snapshot->sample_size);
  put32(footer + 48, crc);
  put32(footer + 52, PERF_FOOTER_SIZE);
  // Complete the 20-byte SHA-1 build ID started at header offset 24.
  if (__perf_build_id_note_end - __perf_build_id_note_start >= 36)
    __memcpy(footer + 56, __perf_build_id_note_start + 32, 4);
}

static void capture_snapshot(uint32_t reason, bool complete) {
  perf_snapshot.early_size = perf_early_committed_bytes();
  unsigned event_bank = perf_snapshot.event_bank ^ 1U;
  uint32_t first_event_sequence =
      perf_snapshot.early_size / XOS_PERF_EARLY_RECORD_SIZE;
  bool event_valid = true;
  perf_snapshot.event_size =
      perf_event_capture(event_bank, first_event_sequence, &event_valid);
  perf_snapshot.event_bank = event_bank;
  unsigned counter_bank = perf_snapshot.counter_bank ^ 1U;
  uint32_t first_counter_sequence =
      (perf_snapshot.early_size + perf_snapshot.event_size) /
      XOS_PERF_EARLY_RECORD_SIZE;
  perf_snapshot.counter_size =
      perf_counter_capture(counter_bank, first_counter_sequence);
  perf_snapshot.counter_bank = counter_bank;
  unsigned bank = perf_snapshot.sample_bank ^ 1U;
  uint32_t first_sequence =
      (perf_snapshot.early_size + perf_snapshot.event_size +
       perf_snapshot.counter_size) /
      XOS_PERF_EARLY_RECORD_SIZE;
  perf_snapshot.sample_size = perf_sample_capture(bank, first_sequence);
  perf_snapshot.sample_bank = bank;
  perf_snapshot.lost_samples = perf_sample_lost();
  perf_snapshot.sample_hits = perf_sample_hits(bank);
  perf_snapshot.end_timestamp = rdtsc64();
  perf_snapshot.end_reason = reason;
  perf_snapshot.complete =
      complete && event_valid && perf_event_lost() == 0 &&
      perf_snapshot.lost_samples == 0 && perf_counter_is_complete() &&
      snapshot_records_size(&perf_snapshot) <= 128ULL * 1024ULL * 1024ULL;
  perf_snapshot.available = true;
}

void perf_register_target(xtask *task) {
  if (__atomic_load_n(&perf_state, __ATOMIC_ACQUIRE) == XOS_PERF_RUNNING) {
    perf_event_reset();
    perf_sample_reset();
    perf_target = task;
  }
}

int perf_freeze(uint32_t reason, bool complete) {
  uint32_t expected = XOS_PERF_RUNNING;
  if (!__atomic_compare_exchange_n(&perf_state, &expected, XOS_PERF_FREEZING,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return expected == XOS_PERF_FROZEN ? 0 : -1;
  spin_lock(&perf_snapshot_lock);
  perf_event_stop();
  perf_sample_stop();
  perf_pmu_stop_cpu();
  perf_counter_capture_final();
  capture_snapshot(reason, complete);
  spin_unlock(&perf_snapshot_lock);
  __atomic_store_n(&perf_state, XOS_PERF_FROZEN, __ATOMIC_RELEASE);
  return 0;
}

void perf_watchdog_tick(void) {
  if (get_cpu_local()->cpu_id == 0 && tsc_freq &&
      rdtsc64() - perf_boot_tsc >= 600ULL * tsc_freq)
    __atomic_store_n(&perf_watchdog_requested, 1U, __ATOMIC_RELEASE);
}

int perf_checkpoint(void) {
  spin_lock(&perf_snapshot_lock);
  if (__atomic_load_n(&perf_state, __ATOMIC_ACQUIRE) != XOS_PERF_RUNNING) {
    spin_unlock(&perf_snapshot_lock);
    return -1;
  }
  capture_snapshot(XOS_PERF_END_NONE, false);
  spin_unlock(&perf_snapshot_lock);
  return 0;
}

void perf_target_exit(xtask *task, int32_t status) {
  if (task != perf_target)
    return;
  uint32_t reason = XOS_PERF_END_TARGET_EXIT;
  if ((status & 0x7f) != 0)
    reason = XOS_PERF_END_TARGET_SIGNAL;
  else if (((status >> 8) & 0xff) != 0)
    reason = XOS_PERF_END_TARGET_FAILURE;
  perf_freeze(reason, status == 0);
}

void perf_get_info(void *out) {
  if (__atomic_exchange_n(&perf_watchdog_requested, 0U, __ATOMIC_ACQ_REL) &&
      __atomic_load_n(&perf_state, __ATOMIC_ACQUIRE) == XOS_PERF_RUNNING)
    perf_freeze(XOS_PERF_END_WATCHDOG, false);
  struct xos_perf_info *info = out;
  __memset(info, 0, sizeof(*info));
  info->size = sizeof(*info);
  info->abi_version = XOS_PERF_ABI_VERSION;
  spin_lock(&perf_snapshot_lock);
  info->state = __atomic_load_n(&perf_state, __ATOMIC_ACQUIRE);
  info->flags = perf_snapshot.complete ? 1U : 0U;
  if (perf_snapshot.available)
    info->raw_size = PERF_HEADER_SIZE + snapshot_records_size(&perf_snapshot) +
                     PERF_FOOTER_SIZE;
  info->end_reason = perf_snapshot.end_reason;
  info->end_timestamp = perf_snapshot.end_timestamp;
  info->lost_samples = perf_snapshot.lost_samples;
  spin_unlock(&perf_snapshot_lock);
}

void perf_get_metadata(void *out) {
  struct xos_perf_metadata *metadata = out;
  __memset(metadata, 0, sizeof(*metadata));
  metadata->size = sizeof(*metadata);
  metadata->abi_version = XOS_PERF_ABI_VERSION;
  metadata->boot_tsc = perf_boot_tsc;
  metadata->tsc_freq = tsc_freq;
  spin_lock(&perf_snapshot_lock);
  metadata->record_count =
      snapshot_records_size(&perf_snapshot) / XOS_PERF_EARLY_RECORD_SIZE;
  metadata->committed_bytes = snapshot_records_size(&perf_snapshot);
  metadata->pmu_active_mask = perf_pmu_active_mask();
  metadata->sampling_source = metadata->pmu_active_mask
                                  ? XOS_PERF_SAMPLE_PMU_NMI
                                  : XOS_PERF_SAMPLE_LAPIC_TIMER;
  metadata->nmi_count = perf_pmu_nmi_count();
  metadata->handler_cycles = perf_pmu_handler_cycles();
  metadata->truncated_callchains = perf_sample_truncated();
  metadata->trace_lost = perf_event_lost();
  spin_unlock(&perf_snapshot_lock);
}

size_t perf_raw_read(uint64_t offset, void *buffer, size_t length) {
  struct perf_snapshot snapshot;
  spin_lock(&perf_snapshot_lock);
  snapshot = perf_snapshot;
  spin_unlock(&perf_snapshot_lock);
  if (!snapshot.available)
    return 0;
  uint8_t header[PERF_HEADER_SIZE];
  uint8_t footer[PERF_FOOTER_SIZE];
  build_header(header, &snapshot);
  build_footer(footer, &snapshot);

  const uint64_t records_size = snapshot_records_size(&snapshot);
  const uint64_t total = PERF_HEADER_SIZE + records_size + PERF_FOOTER_SIZE;
  if (offset >= total)
    return 0;
  if (length > total - offset)
    length = (size_t)(total - offset);

  uint8_t *dst = buffer;
  size_t copied = 0;
  while (copied < length) {
    uint64_t position = offset + copied;
    const uint8_t *source;
    size_t available;
    if (position < PERF_HEADER_SIZE) {
      source = header + position;
      available = PERF_HEADER_SIZE - (size_t)position;
    } else if (position < PERF_HEADER_SIZE + snapshot.early_size) {
      uint64_t record_offset = position - PERF_HEADER_SIZE;
      source = perf_early_data() + record_offset;
      available = (size_t)(snapshot.early_size - record_offset);
    } else if (position <
               PERF_HEADER_SIZE + snapshot.early_size + snapshot.event_size) {
      uint64_t event_offset = position - PERF_HEADER_SIZE - snapshot.early_size;
      source = perf_event_data(snapshot.event_bank) + event_offset;
      available = (size_t)(snapshot.event_size - event_offset);
    } else if (position < PERF_HEADER_SIZE + records_size) {
      uint64_t sample_offset = position - PERF_HEADER_SIZE -
                               snapshot.early_size - snapshot.event_size;
      if (sample_offset < snapshot.counter_size) {
        source = perf_counter_data(snapshot.counter_bank) + sample_offset;
        available = (size_t)(snapshot.counter_size - sample_offset);
      } else {
        sample_offset -= snapshot.counter_size;
        source = perf_sample_data(snapshot.sample_bank) + sample_offset;
        available = (size_t)(snapshot.sample_size - sample_offset);
      }
    } else {
      uint64_t footer_offset = position - PERF_HEADER_SIZE - records_size;
      source = footer + footer_offset;
      available = PERF_FOOTER_SIZE - (size_t)footer_offset;
    }
    if (available > length - copied)
      available = length - copied;
    __memcpy(dst + copied, source, available);
    copied += available;
  }
  return copied;
}

#endif
