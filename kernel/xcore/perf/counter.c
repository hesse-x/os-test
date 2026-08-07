/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/perf/counter.h"

#ifdef PERF

#include <stdbool.h>

#include <xos/errno.h>
#include <xos/perf.h>

#include "arch/x64/utils.h"
#include "kernel/xcore/perf/event.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/spinlock.h"

#define COUNTER_SNAPSHOT_MAX 6U
#define COUNTER_VALUE_MAX 160U
#define COUNTER_RECORD_MAX (COUNTER_SNAPSHOT_MAX * (COUNTER_VALUE_MAX + 3U))

enum counter_id {
  C_EVENT_CPU_BASE = 128,
  C_WAKE_VALID_BASE = 160,
  C_WAKE_NOOP_BASE = 172,
  C_WAKE_CROSS_CPU_IPI = 184,
  C_WAKE_SPURIOUS_CANCELS = 185,
};

struct counter_value {
  uint16_t id;
  uint64_t value;
};

struct counter_snapshot {
  uint64_t begin_tsc;
  uint64_t end_tsc;
  uint64_t availability;
  uint16_t mark_id;
  uint16_t nr_values;
  struct counter_value values[COUNTER_VALUE_MAX];
};

struct counter_record {
  uint64_t payload;
  uint32_t sequence;
  uint16_t ident;
  uint8_t kind;
  uint8_t aux;
  uint32_t committed_size;
  uint32_t value;
};

_Static_assert(sizeof(struct counter_record) == XOS_PERF_EARLY_RECORD_SIZE,
               "counter record ABI size");

static struct counter_snapshot snapshots[COUNTER_SNAPSHOT_MAX];
static struct counter_record records[2][COUNTER_RECORD_MAX];
static spinlock counter_lock = SPINLOCK_INIT;
static uint16_t nr_snapshots;
static bool final_complete;
static perf_counter_collector_fn external_collector;

static void add(struct counter_snapshot *snapshot, uint16_t id,
                uint64_t value) {
  if (snapshot->nr_values >= COUNTER_VALUE_MAX)
    return;
  snapshot->values[snapshot->nr_values++] =
      (struct counter_value){.id = id, .value = value};
}

static void writer_add(void *context, uint16_t id, uint64_t value) {
  add(context, id, value);
}

static void writer_set_available(void *context, uint64_t providers) {
  struct counter_snapshot *snapshot = context;
  snapshot->availability |= providers;
}

void perf_counter_register_collector(perf_counter_collector_fn collector) {
  __atomic_store_n(&external_collector, collector, __ATOMIC_RELEASE);
}

static void collect(struct counter_snapshot *snapshot, uint16_t mark_id) {
  snapshot->begin_tsc = rdtsc64();
  snapshot->mark_id = mark_id;
  snapshot->availability =
      PERF_COUNTER_PROVIDER_EVENT | PERF_COUNTER_PROVIDER_SCHED;

  perf_counter_collector_fn collector =
      __atomic_load_n(&external_collector, __ATOMIC_ACQUIRE);
  if (collector) {
    const struct perf_counter_writer writer = {
        .context = snapshot,
        .add = writer_add,
        .set_available = writer_set_available,
    };
    collector(&writer);
  }

  struct sched_wake_stats wake;
  sched_get_wake_stats(&wake);
  for (unsigned event = 0; event < SCHED_WAIT_EVENT_COUNT; event++) {
    add(snapshot, C_WAKE_VALID_BASE + event, wake.valid[event]);
    add(snapshot, C_WAKE_NOOP_BASE + event, wake.noop[event]);
  }
  add(snapshot, C_WAKE_CROSS_CPU_IPI, wake.cross_cpu_ipi);
  add(snapshot, C_WAKE_SPURIOUS_CANCELS, wake.spurious_cancels);
  for (unsigned cpu = 0; cpu < perf_event_cpu_count(); cpu++) {
    struct perf_event_cpu_stats event;
    perf_event_get_cpu_stats(cpu, &event);
    uint16_t base = C_EVENT_CPU_BASE + (uint16_t)(cpu * 4U);
    add(snapshot, base, event.capacity);
    add(snapshot, base + 1, event.attempted);
    add(snapshot, base + 2, event.committed);
    add(snapshot, base + 3, event.high_water);
  }
  snapshot->end_tsc = rdtsc64();
}

int perf_counter_mark(uint16_t id) {
  if (id < XOS_PERF_GUI_START || id > XOS_PERF_GUI_SHELL_READY)
    return -EINVAL;
  spin_lock(&counter_lock);
  if (id <= nr_snapshots) {
    spin_unlock(&counter_lock);
    return -EALREADY;
  }
  if (id != nr_snapshots + 1U) {
    spin_unlock(&counter_lock);
    return -EINVAL;
  }
  collect(&snapshots[nr_snapshots], id);
  nr_snapshots++;
  spin_unlock(&counter_lock);
  return 0;
}

void perf_counter_capture_final(void) {
  spin_lock(&counter_lock);
  final_complete = nr_snapshots == XOS_PERF_GUI_SHELL_READY;
  if (nr_snapshots < COUNTER_SNAPSHOT_MAX) {
    collect(&snapshots[nr_snapshots], XOS_PERF_COUNTER_FINAL);
    nr_snapshots++;
  }
  spin_unlock(&counter_lock);
}

bool perf_counter_is_complete(void) {
  return __atomic_load_n(&final_complete, __ATOMIC_ACQUIRE);
}

static void write_record(struct counter_record *record, uint64_t payload,
                         uint32_t sequence, uint16_t ident, uint8_t kind,
                         uint8_t aux, uint32_t value) {
  *record = (struct counter_record){.payload = payload,
                                    .sequence = sequence,
                                    .ident = ident,
                                    .kind = kind,
                                    .aux = aux,
                                    .committed_size = sizeof(*record),
                                    .value = value};
}

uint32_t perf_counter_capture(unsigned bank, uint32_t first_sequence) {
  bank &= 1U;
  uint32_t out = 0;
  spin_lock(&counter_lock);
  for (uint16_t i = 0; i < nr_snapshots; i++) {
    const struct counter_snapshot *snapshot = &snapshots[i];
    uint8_t id = (uint8_t)(i + 1U);
    write_record(&records[bank][out], snapshot->begin_tsc, first_sequence + out,
                 snapshot->mark_id, XOS_PERF_COUNTER_BEGIN, id,
                 snapshot->nr_values);
    out++;
    write_record(&records[bank][out], snapshot->availability,
                 first_sequence + out, 0, XOS_PERF_COUNTER_AVAILABILITY, id, 0);
    out++;
    for (uint16_t value = 0; value < snapshot->nr_values; value++) {
      write_record(&records[bank][out], snapshot->values[value].value,
                   first_sequence + out, snapshot->values[value].id,
                   XOS_PERF_COUNTER_VALUE, id, 0);
      out++;
    }
    write_record(&records[bank][out], snapshot->end_tsc, first_sequence + out,
                 snapshot->mark_id, XOS_PERF_COUNTER_END, id, 0);
    out++;
  }
  spin_unlock(&counter_lock);
  return out * sizeof(struct counter_record);
}

const uint8_t *perf_counter_data(unsigned bank) {
  return (const uint8_t *)records[bank & 1U];
}

#endif
