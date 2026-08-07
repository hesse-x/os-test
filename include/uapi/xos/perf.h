/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef XOS_UAPI_PERF_H
#define XOS_UAPI_PERF_H

#include <stdint.h>

#define XOS_PERF_ABI_VERSION 1U
#define XOS_PERF_RAW_MAJOR 1U
#define XOS_PERF_RAW_MINOR 3U
#define XOS_PERF_MAX_READ (128U * 1024U)
#define XOS_PERF_FILE_HEADER_SIZE 88U
#define XOS_PERF_EARLY_RECORD_SIZE 24U
#define XOS_PERF_FILE_FOOTER_SIZE 64U

enum xos_perf_cmd {
  XOS_PERF_GET_INFO = 0,
  XOS_PERF_REGISTER_TARGET = 1,
  XOS_PERF_MARK = 2,
  XOS_PERF_FREEZE = 3,
  XOS_PERF_READ = 4,
  XOS_PERF_GET_METADATA = 5,
  XOS_PERF_REQUEST_EXIT = 6,
  XOS_PERF_CHECKPOINT = 7,
  XOS_PERF_COUNTER_MARK = 8,
};

enum xos_perf_state {
  XOS_PERF_RUNNING = 1,
  XOS_PERF_FREEZING = 2,
  XOS_PERF_FROZEN = 3,
};

enum xos_perf_end_reason {
  XOS_PERF_END_NONE = 0,
  XOS_PERF_END_TARGET_EXIT = 1,
  XOS_PERF_END_TARGET_FAILURE = 2,
  XOS_PERF_END_TARGET_SIGNAL = 3,
  XOS_PERF_END_MANUAL = 4,
  XOS_PERF_END_WATCHDOG = 5,
};

enum xos_perf_event_type {
  XOS_PERF_PHASE_BEGIN = 1,
  XOS_PERF_PHASE_END = 2,
  XOS_PERF_TRACE_ERROR = 3,
  XOS_PERF_MARK_EVENT = 4,
  /* Aggregated kernel RIP samples: timestamp=RIP, value=hit count. */
  XOS_PERF_SAMPLE_AGG = 5,
  XOS_PERF_TRACE_EVENT = 6,
  XOS_PERF_CALLCHAIN = 7,
  XOS_PERF_CALLCHAIN_FRAME = 8,
  XOS_PERF_COUNTER_BEGIN = 9,
  XOS_PERF_COUNTER_AVAILABILITY = 10,
  XOS_PERF_COUNTER_VALUE = 11,
  XOS_PERF_COUNTER_END = 12,
};

enum xos_perf_counter_mark {
  XOS_PERF_GUI_START = 1,
  XOS_PERF_GUI_COMPOSITOR_READY = 2,
  XOS_PERF_GUI_TERMINAL_XDG_READY = 3,
  XOS_PERF_GUI_TERMINAL_FIRST_BUFFER = 4,
  XOS_PERF_GUI_SHELL_READY = 5,
  XOS_PERF_COUNTER_FINAL = 6,
};

enum xos_perf_sample_source {
  XOS_PERF_SAMPLE_LAPIC_TIMER = 1,
  XOS_PERF_SAMPLE_PMU_NMI = 2,
};

enum xos_perf_unwind_stop {
  XOS_PERF_UNWIND_COMPLETE = 0,
  XOS_PERF_UNWIND_DEPTH = 1,
  XOS_PERF_UNWIND_BAD_FRAME = 2,
  XOS_PERF_UNWIND_UNMAPPED = 3,
  XOS_PERF_UNWIND_BAD_RETURN = 4,
};

enum xos_perf_trace_type {
  XOS_PERF_TRACE_SCHED_SWITCH = 1,
  XOS_PERF_TRACE_TASK_BLOCK = 2,
  XOS_PERF_TRACE_TASK_WAKE = 3,
  XOS_PERF_TRACE_IRQ_BEGIN = 4,
  XOS_PERF_TRACE_IRQ_END = 5,
  XOS_PERF_TRACE_IPC = 6,
  XOS_PERF_TRACE_IO = 7,
  /* Snapshot records: payload=count/total cycles/max cycles, value=owner. */
  XOS_PERF_TRACE_IRQ_COUNT = 8,
  XOS_PERF_TRACE_IRQ_TOTAL = 9,
  XOS_PERF_TRACE_IRQ_MAX = 10,
  /* value=I/O cookie, subtype=IRQ vector for an in-handler completion. */
  XOS_PERF_TRACE_IRQ_CAUSE = 11,
};

enum xos_perf_ipc_stage {
  XOS_PERF_IPC_SEND = 1,
  XOS_PERF_IPC_RECEIVE = 2,
  XOS_PERF_IPC_REPLY = 3,
  XOS_PERF_IPC_WAKE = 4,
  XOS_PERF_IPC_ENQUEUE = 5,
  XOS_PERF_IPC_DEQUEUE = 6,
};

enum xos_perf_io_stage {
  XOS_PERF_IO_SUBMIT = 1,
  XOS_PERF_IO_COMPLETE = 2,
  XOS_PERF_IO_WAKE = 3,
  XOS_PERF_IO_RESUME = 4,
};

enum xos_perf_mark_stage {
  XOS_PERF_MARK_BEGIN = 1,
  XOS_PERF_MARK_END = 2,
};

enum xos_perf_mark_status {
  XOS_PERF_MARK_STATUS_NONE = 0,
  XOS_PERF_MARK_STATUS_PASS = 1,
  XOS_PERF_MARK_STATUS_FAIL = 2,
  XOS_PERF_MARK_STATUS_SKIP = 3,
  XOS_PERF_MARK_STATUS_CRASH = 4,
};

struct xos_perf_info {
  uint32_t size;
  uint32_t abi_version;
  uint32_t state;
  uint32_t flags;
  uint64_t raw_size;
  uint64_t end_reason;
  uint64_t end_timestamp;
  uint64_t lost_critical;
  uint64_t lost_samples;
};

struct xos_perf_metadata {
  uint32_t size;
  uint32_t abi_version;
  uint64_t boot_tsc;
  uint64_t tsc_freq;
  uint64_t record_count;
  uint64_t committed_bytes;
  uint32_t sampling_source;
  uint32_t pmu_active_mask;
  uint64_t nmi_count;
  uint64_t handler_cycles;
  uint64_t truncated_callchains;
  uint64_t trace_lost;
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct xos_perf_info) == 56, "xos_perf_info ABI");
_Static_assert(sizeof(struct xos_perf_metadata) == 80, "xos_perf_metadata ABI");
#endif

#endif
