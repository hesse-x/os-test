#!/usr/bin/env python3
"""Validate, symbolize and analyze an XOS performance snapshot."""

import argparse
import json
import re
import struct
import subprocess
import sys
import zlib
from collections import defaultdict
from pathlib import Path

HEADER_SIZE = 88
FOOTER_SIZE = 64
RECORD_SIZE = 24
MAX_RAW_SIZE = 128 * 1024 * 1024
KERNEL_BASE = 0xFFFFFF8000000000
TRACE_SCHED_SWITCH, TRACE_TASK_BLOCK, TRACE_TASK_WAKE = 1, 2, 3
TRACE_IRQ_BEGIN, TRACE_IRQ_END, TRACE_IPC, TRACE_IO = 4, 5, 6, 7
TRACE_IRQ_COUNT, TRACE_IRQ_TOTAL, TRACE_IRQ_MAX = 8, 9, 10
TRACE_IRQ_CAUSE = 11
TRACE_EXEC = 12
IRQ_SUMMARY_TYPES = {TRACE_IRQ_COUNT, TRACE_IRQ_TOTAL, TRACE_IRQ_MAX}
TIMED_TRACE_TYPES = {TRACE_SCHED_SWITCH, TRACE_TASK_BLOCK, TRACE_TASK_WAKE,
                     TRACE_IRQ_BEGIN, TRACE_IRQ_END, TRACE_IPC, TRACE_IO,
                     TRACE_IRQ_CAUSE, TRACE_EXEC}
SOURCE_NAMES = {1: "lapic_timer", 2: "pmu_nmi"}
IPC_STAGES = {1: "send", 2: "receive", 3: "reply", 4: "wake",
              5: "enqueue", 6: "dequeue"}
IO_STAGES = {1: "submit", 2: "complete", 3: "wake"}
IO_STAGES[4] = "resume"

COUNTER_NAMES = {
    1: "block.submitted", 2: "block.completed", 3: "block.failed",
    4: "block.validation_rejected", 5: "block.read_commands",
    6: "block.write_commands", 7: "block.read_sectors",
    8: "block.write_sectors", 9: "block.read_bucket_1",
    10: "block.read_bucket_2_7", 11: "block.read_bucket_8",
    12: "block.read_bucket_9_127", 13: "block.read_bucket_128",
    14: "block.write_bucket_1", 15: "block.write_bucket_2_7",
    16: "block.write_bucket_8", 17: "block.write_bucket_9_127",
    18: "block.write_bucket_128", 32: "fat.cache_hits",
    33: "fat.cache_misses", 34: "fat.cache_fill_waits",
    35: "fat.cache_io_commands", 36: "fat.cache_io_sectors",
    37: "fat.demand_calls", 38: "fat.demand_steps",
    39: "fat.demand_head_restarts", 40: "fat.demand_backtracks",
    41: "fat.demand_invalid", 42: "fat.demand_mapped_sectors",
    43: "fat.readahead_calls", 44: "fat.readahead_steps",
    45: "fat.readahead_head_restarts", 46: "fat.readahead_backtracks",
    47: "fat.readahead_invalid", 48: "fat.readahead_mapped_sectors",
    49: "fat.map_bytes", 50: "fat.map_peak_bytes",
    51: "fat.map_peak_inode_bytes",
    64: "readahead.batches", 65: "readahead.pages",
    66: "readahead.hits", 67: "readahead.waste",
    68: "readahead.fragment_truncations", 69: "readahead.fallbacks",
    70: "counter.capacity", 71: "counter.count", 72: "counter.overflow",
    80: "ahci.sync_submitted", 81: "ahci.async_submitted",
    82: "ahci.completed", 83: "ahci.errors", 84: "ahci.sync_wakes",
    85: "ahci.async_wakes", 86: "ahci.early_completes",
    87: "ahci.cross_cpu_wakes", 88: "ahci.queue_full",
    89: "ahci.invalid_timing", 90: "ahci.queue_wait_count",
    91: "ahci.queue_wait_cycles", 92: "ahci.queue_wait_max",
    93: "ahci.service_count", 94: "ahci.service_cycles",
    95: "ahci.service_max", 96: "ahci.queue_depth_cycles",
    97: "ahci.queue_depth", 98: "ahci.queue_depth_max",
}
MARK_NAMES = {1: "gui_start", 2: "compositor_ready",
              3: "terminal_xdg_ready", 4: "terminal_first_buffer",
              5: "shell_ready", 6: "final"}
WAIT_NAMES = ("none", "recv", "req_reply", "child", "msg_reply", "poll",
              "futex", "vfork", "pause", "sleep", "block_io", "mutex",
              "completion", "kthread")
EXEC_NAMES = {1: "clang", 2: "clang -cc1", 3: "ld.lld"}


def counter_name(ident):
    if ident in COUNTER_NAMES:
        return COUNTER_NAMES[ident]
    if 160 <= ident < 172:
        return f"wake.valid_{WAIT_NAMES[ident - 160]}"
    if 172 <= ident < 184:
        return f"wake.noop_{WAIT_NAMES[ident - 172]}"
    if ident == 184:
        return "wake.cross_cpu_ipi"
    if ident == 185:
        return "wake.spurious_cancels"
    if 186 <= ident < 190:
        event = 12 + (ident - 186) // 2
        outcome = "valid" if (ident - 186) % 2 == 0 else "noop"
        return f"wake.{outcome}_{WAIT_NAMES[event]}"
    if 200 <= ident < 232:
        return f"ahci.queue_wait_hist_{ident - 200}"
    if 232 <= ident < 264:
        return f"ahci.service_hist_{ident - 232}"
    if 300 <= ident < 364:
        source_index, field_index = divmod(ident - 300, 32)
        if source_index < 2:
            source = ("mmap", "read")[source_index]
            fields = ("calls", "requested_pages", "admitted_demand",
                      "admitted_speculative", "hits", "eviction_waste",
                      "invalidation_waste", "outstanding", "outstanding_peak")
            if field_index < len(fields):
                return f"readahead.{source}.{fields[field_index]}"
            bucket_names = ("1", "4", "8", "16", "other")
            if 9 <= field_index < 14:
                return f"readahead.{source}.requested_window_{bucket_names[field_index - 9]}"
            if 14 <= field_index < 19:
                return f"readahead.{source}.effective_window_{bucket_names[field_index - 14]}"
            if 19 <= field_index < 24:
                return f"readahead.{source}.admitted_window_{bucket_names[field_index - 19]}"
            detail = ("fragment_truncations", "reservation_conflicts",
                      "staging_fallbacks", "batch_io_commands",
                      "batch_io_sectors")
            if 24 <= field_index < 29:
                return f"readahead.{source}.{detail[field_index - 24]}"
    if 400 <= ident < 720:
        stage_index, field_index = divmod(ident - 400, 40)
        stages = ("ack", "lock_wait", "bookkeeping", "copy", "wake",
                  "next_submit", "unlock_exit", "locked_total")
        if stage_index < len(stages):
            stage = stages[stage_index]
            if field_index < 3:
                return f"ahci_irq.{stage}." + ("count", "cycles", "max")[field_index]
            if field_index < 35:
                return f"ahci_irq.{stage}.hist_{field_index - 3}"
    if 720 <= ident < 723:
        return "ahci_irq.handler_total." + ("count", "cycles", "max")[ident - 720]
    if 723 <= ident < 755:
        return f"ahci_irq.handler_total.hist_{ident - 723}"
    if ident == 755:
        return "ahci_irq.spurious.count"
    if ident == 756:
        return "ahci_irq.spurious.cycles"
    if ident == 757:
        return "ahci_irq.orphan.count"
    if ident == 758:
        return "ahci_irq.orphan.cycles"
    if 759 <= ident < 789:
        threshold_index, field_index = divmod(ident - 759, 15)
        threshold = ("100us", "1ms")[threshold_index]
        stages = ("ack", "lock_wait", "bookkeeping", "copy", "wake",
                  "next_submit", "unlock_exit")
        if field_index == 0:
            return f"ahci_irq.long_tail_{threshold}.count"
        if 1 <= field_index < 8:
            return f"ahci_irq.long_tail_{threshold}.{stages[field_index - 1]}_cycles"
        return f"ahci_irq.long_tail_{threshold}.{stages[field_index - 8]}_max"
    if 128 <= ident < 144:
        cpu, field = divmod(ident - 128, 4)
        return f"event.cpu{cpu}." + ("capacity", "attempted", "committed",
                                     "high_water")[field]
    return f"unknown.{ident}"

PHASE_NAMES = {
    1: "boot_to_kernel_main", 2: "early_paging", 3: "early_gdt",
    4: "early_higher_half", 16: "xcore", 17: "memory", 18: "acpi",
    19: "idt", 20: "apic_tsc", 21: "scheduler", 22: "smp",
    32: "vfs_core", 33: "inode", 34: "page_cache", 35: "devtmpfs",
    48: "driver", 49: "pci", 50: "ahci", 51: "xhci", 52: "drm",
    64: "bsd", 80: "init_elf", 96: "init", 97: "service_syslogd",
    98: "service_evdev", 99: "service_udevd", 100: "service_seatd",
    101: "service_compositor", 102: "service_desktop",
    103: "service_shell", 112: "test_runner", 113: "test_case",
}
MARK_STATUS = {0: "none", 1: "pass", 2: "fail", 3: "skip", 4: "crash"}


class PerfFormatError(Exception):
    pass


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def signed16(value):
    return value - 0x10000 if value & 0x8000 else value


def signed32(value):
    return value - 0x100000000 if value & 0x80000000 else value


def raw_record(blob, sequence):
    offset = sequence * RECORD_SIZE
    fields = struct.unpack_from("<QIHBBII", blob, offset)
    first, actual, ident, kind, aux, committed, value = fields
    raw_offset = HEADER_SIZE + offset
    if committed != RECORD_SIZE:
        raise PerfFormatError(f"uncommitted record at raw offset {raw_offset}")
    if actual != sequence:
        raise PerfFormatError(f"record sequence mismatch at raw offset {raw_offset}")
    return first, ident, kind, aux, value, raw_offset


def parse_raw(path):
    if path.stat().st_size > MAX_RAW_SIZE:
        raise PerfFormatError(f"raw file exceeds {MAX_RAW_SIZE} bytes")
    data = path.read_bytes()
    if len(data) < HEADER_SIZE + FOOTER_SIZE or data[:8] != b"XOSPERF\0":
        raise PerfFormatError("invalid or truncated raw header")
    major, minor = u16(data, 8), u16(data, 10)
    if major != 1 or minor not in (0, 1, 2, 3, 4):
        raise PerfFormatError(f"unsupported raw ABI version {major}.{minor}")
    if data[12] != 1 or data[13] != 64 or u16(data, 14) != HEADER_SIZE:
        raise PerfFormatError("unsupported endian, pointer width, or header size")
    header = bytearray(data[:HEADER_SIZE])
    expected_crc = u32(header, 80)
    header[80:84] = b"\0\0\0\0"
    if zlib.crc32(header) != expected_crc:
        raise PerfFormatError("header CRC32 mismatch")

    records_size = u64(data, 72)
    if records_size % RECORD_SIZE:
        raise PerfFormatError("record section is not 24-byte aligned")
    footer_offset = HEADER_SIZE + records_size
    if footer_offset + FOOTER_SIZE != len(data):
        raise PerfFormatError("record bounds do not match raw file size")
    footer = data[footer_offset:]
    if footer[:8] != b"XOSEND\0\0" or u32(footer, 52) != FOOTER_SIZE:
        raise PerfFormatError("invalid footer magic or size")
    record_count = u64(footer, 16)
    if u64(footer, 8) != records_size or record_count != records_size // RECORD_SIZE:
        raise PerfFormatError("header/footer record counts differ")
    blob = data[HEADER_SIZE:footer_offset]
    if zlib.crc32(blob) != u32(footer, 48):
        raise PerfFormatError("record CRC32 mismatch")

    boot_tsc, tsc_freq, end_tsc = u64(data, 40), u64(data, 48), u64(data, 64)
    if not tsc_freq or end_tsc < boot_tsc:
        raise PerfFormatError("invalid session timestamps")
    records, events, chains, counter_build = [], [], [], {}
    previous_tsc = 0
    sequence = 0
    while sequence < record_count:
        first, ident, kind, aux, value, raw_offset = raw_record(blob, sequence)
        if kind in (1, 2, 3, 4):
            if first < boot_tsc or first < previous_tsc or first > end_tsc:
                raise PerfFormatError(f"invalid timestamp at raw offset {raw_offset}")
            previous_tsc = first
            records.append({"timestamp": first, "id": ident, "kind": kind,
                            "aux": aux, "value": value})
        elif kind == 5:
            if minor < 1 or first < KERNEL_BASE or value == 0:
                raise PerfFormatError(f"invalid legacy sample at raw offset {raw_offset}")
            chains.append({"cpu": 0, "source": aux, "count": value,
                           "frames": [first], "unwind_stop": 0})
        elif kind == 6:
            trace_type = ident & 0xff
            if (minor < 2 or trace_type not in TIMED_TRACE_TYPES | IRQ_SUMMARY_TYPES or
                    (trace_type == TRACE_EXEC and minor < 4) or
                    (trace_type in TIMED_TRACE_TYPES and
                     (first < boot_tsc or first > end_tsc))):
                raise PerfFormatError(f"invalid trace event at raw offset {raw_offset}")
            events.append({"timestamp": first, "type": trace_type,
                           "subtype": ident >> 8, "cpu": aux, "value": value})
        elif kind == 7:
            depth, cpu = ident & 0xff, ident >> 8
            if minor < 2 or depth == 0 or depth > 32 or value == 0:
                raise PerfFormatError(f"invalid callchain at raw offset {raw_offset}")
            frames, stop = [], 0
            for frame_index in range(depth):
                sequence += 1
                if sequence >= record_count:
                    raise PerfFormatError("callchain is truncated")
                address, frame_id, frame_kind, frame_aux, frame_value, frame_offset = \
                    raw_record(blob, sequence)
                if frame_kind != 8 or frame_id != frame_index or frame_value != 0:
                    raise PerfFormatError(f"invalid callchain frame at raw offset {frame_offset}")
                if address == 0:
                    raise PerfFormatError(f"zero callchain address at raw offset {frame_offset}")
                frames.append(address)
                if frame_index + 1 == depth:
                    stop = frame_aux
            chains.append({"cpu": cpu, "source": aux, "count": value,
                           "hash": first, "frames": frames,
                           "unwind_stop": stop})
        elif kind == 9:
            if minor < 3 or aux == 0 or aux > 6 or ident not in MARK_NAMES or aux in counter_build:
                raise PerfFormatError(f"invalid counter begin at raw offset {raw_offset}")
            if first < boot_tsc or first > end_tsc:
                raise PerfFormatError(f"invalid counter timestamp at raw offset {raw_offset}")
            counter_build[aux] = {"id": aux, "mark_id": ident,
                                  "name": MARK_NAMES[ident], "begin_tsc": first,
                                  "expected": value, "values": {},
                                  "availability": None, "end_tsc": None}
        elif kind == 10:
            snapshot = counter_build.get(aux)
            if minor < 3 or not snapshot or ident != 0 or value != 0 or snapshot["availability"] is not None:
                raise PerfFormatError(f"invalid counter availability at raw offset {raw_offset}")
            snapshot["availability"] = first
        elif kind == 11:
            snapshot = counter_build.get(aux)
            if minor < 3 or not snapshot or value != 0 or snapshot["end_tsc"] is not None or ident in snapshot["values"]:
                raise PerfFormatError(f"invalid counter value at raw offset {raw_offset}")
            snapshot["values"][ident] = first
        elif kind == 12:
            snapshot = counter_build.get(aux)
            if (minor < 3 or not snapshot or ident != snapshot["mark_id"] or value != 0 or
                    snapshot["end_tsc"] is not None or first < snapshot["begin_tsc"] or first > end_tsc):
                raise PerfFormatError(f"invalid counter end at raw offset {raw_offset}")
            snapshot["end_tsc"] = first
        else:
            raise PerfFormatError(f"unknown record type {kind} at raw offset {raw_offset}")
        sequence += 1

    sample_hits = sum(chain["count"] for chain in chains)
    if minor >= 1 and sample_hits != u64(footer, 40):
        raise PerfFormatError("sample hit count does not match footer")
    build_id = ((data[24:40] + footer[56:60]).hex()
                if minor >= 2 else None)
    if minor >= 2 and build_id == "0" * 40:
        raise PerfFormatError("raw file has an empty kernel build ID")
    counters = []
    for snapshot_id in sorted(counter_build):
        item = counter_build[snapshot_id]
        if item["availability"] is None or item["end_tsc"] is None:
            raise PerfFormatError(f"counter snapshot {snapshot_id} is incomplete")
        if len(item["values"]) != item["expected"]:
            raise PerfFormatError(f"counter snapshot {snapshot_id} value count mismatch")
        if item["mark_id"] != snapshot_id:
            raise PerfFormatError(f"counter snapshot {snapshot_id} is out of order")
        item["values"] = {
            counter_name(ident): counter
            for ident, counter in item["values"].items()
        }
        counters.append(item)
    if minor >= 3 and bool(u32(data, 16) & 1) and (not counters or counters[-1]["mark_id"] != 6):
        raise PerfFormatError("final counter snapshot is missing")
    return {
        "abi_version": 1, "raw_minor": minor,
        "complete": bool(u32(data, 16) & 1), "boot_tsc": boot_tsc,
        "tsc_freq": tsc_freq, "end_tsc": end_tsc,
        "end_reason": u32(footer, 24), "record_count": record_count,
        "records": records, "events": events, "chains": chains,
        "lost_samples": u64(footer, 32) if minor >= 1 else 0,
        "sample_hits": sample_hits, "kernel_build_id": build_id,
        "counter_snapshots": counters,
    }


def read_test_manifest(path):
    if not path.is_file():
        return {}
    names = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3:
            raise PerfFormatError(f"invalid test manifest row {line_number}: {path}")
        ident = int(fields[0])
        if ident <= 0 or ident > 0xffff or ident in names or not fields[1]:
            raise PerfFormatError(f"invalid test id/name at row {line_number}: {path}")
        names[ident] = fields[1]
    return names


def analyze_phases(snapshot, test_names):
    stack, phases, errors, tests, trace = [], [], [], {}, []
    for record in snapshot["records"]:
        if record["kind"] == 1:
            stack.append({"id": record["id"], "start": record["timestamp"],
                          "child_ticks": 0})
        elif record["kind"] == 2:
            if not stack or stack[-1]["id"] != record["id"]:
                errors.append(f"unmatched phase END id=0x{record['id']:04x}")
                continue
            current = stack.pop()
            inclusive = record["timestamp"] - current["start"]
            name = PHASE_NAMES.get(current["id"], f"phase_0x{current['id']:04x}")
            phases.append({"id": current["id"], "name": name,
                           "inclusive_ticks": inclusive,
                           "exclusive_ticks": inclusive - current["child_ticks"]})
            trace.append({"name": name, "category": "phase", "cpu": 0,
                          "start": current["start"], "duration": inclusive})
            if stack:
                stack[-1]["child_ticks"] += inclusive
        elif record["kind"] == 3:
            errors.append(f"kernel TRACE_ERROR id=0x{record['id']:04x}")
        elif record["aux"] == 1:
            tests[record["id"]] = {"start": record["timestamp"], "status": "running",
                                    "name": test_names.get(record["id"],
                                                           f"test_{record['id']}")}
        elif record["aux"] == 2:
            test = tests.setdefault(record["id"], {
                "start": record["timestamp"],
                "name": test_names.get(record["id"], f"test_{record['id']}")})
            test["end"] = record["timestamp"]
            test["status"] = MARK_STATUS.get(record["value"], "unknown")
            trace.append({"name": test["name"], "category": "test",
                          "cpu": 0, "start": test["start"],
                          "duration": test["end"] - test["start"]})
    errors.extend(f"unclosed phase BEGIN id=0x{x['id']:04x}" for x in stack)
    ms = 1000.0 / snapshot["tsc_freq"]
    phase_rows = [{"id": x["id"], "name": x["name"],
                   "inclusive_ms": x["inclusive_ticks"] * ms,
                   "exclusive_ms": x["exclusive_ticks"] * ms} for x in phases]
    test_rows = [{"id": ident, "name": test["name"], "status": test["status"],
                  "duration_ms": (test.get("end", test["start"]) - test["start"]) * ms}
                 for ident, test in sorted(tests.items())]
    return phase_rows, test_rows, errors, trace


def analyze_events(snapshot):
    events = sorted((event for event in snapshot["events"]
                     if event["type"] not in IRQ_SUMMARY_TYPES),
                    key=lambda event: event["timestamp"])
    ms = 1000.0 / snapshot["tsc_freq"]
    switches, blocks, pending_wakes = (defaultdict(list), defaultdict(list),
                                       defaultdict(list))
    wake_latencies = []
    irq_open, irq_rows, causal = defaultdict(list), [], defaultdict(list)
    exec_rows = []
    irq_summaries = defaultdict(dict)
    trace = []
    for event in snapshot["events"]:
        if event["type"] in IRQ_SUMMARY_TYPES:
            key = (event["cpu"], event["subtype"], signed32(event["value"]))
            field = {TRACE_IRQ_COUNT: "count", TRACE_IRQ_TOTAL: "total_cycles",
                     TRACE_IRQ_MAX: "max_cycles"}[event["type"]]
            irq_summaries[key][field] = event["timestamp"]
    for event in events:
        kind, subtype, value = event["type"], event["subtype"], event["value"]
        if kind == TRACE_SCHED_SWITCH:
            prev, next_pid = signed16(value >> 16), signed16(value & 0xffff)
            switches[event["cpu"]].append({"timestamp": event["timestamp"],
                                            "prev": prev, "next": next_pid,
                                            "next_idle": bool(subtype & 0x10)})
            if pending_wakes[next_pid]:
                wake = pending_wakes[next_pid].pop()
                wake_latencies.append({"pid": next_pid,
                                       "wait_event": wake["subtype"],
                                       "cpu": event["cpu"],
                                       "latency_ms": (event["timestamp"] -
                                                      wake["timestamp"]) * ms})
        elif kind == TRACE_TASK_BLOCK:
            blocks[signed16(value & 0xffff)].append(event)
        elif kind == TRACE_TASK_WAKE:
            pid = signed16(value & 0xffff)
            pending_wakes[pid].append(event)
        elif kind == TRACE_IRQ_BEGIN:
            vector = value & 0xffff
            irq_open[(event["cpu"], vector)].append(event)
        elif kind == TRACE_IRQ_END:
            vector = value & 0xffff
            key = (event["cpu"], vector)
            if irq_open[key]:
                begin = irq_open[key].pop()
                duration = event["timestamp"] - begin["timestamp"]
                irq_rows.append({"cpu": event["cpu"], "vector": vector,
                                 "owner": signed16(value >> 16),
                                 "duration_ms": duration * ms})
                trace.append({"name": f"irq_0x{vector:02x}", "category": "irq",
                              "cpu": event["cpu"], "start": begin["timestamp"],
                              "duration": duration})
        elif kind in (TRACE_IPC, TRACE_IO):
            stages = IPC_STAGES if kind == TRACE_IPC else IO_STAGES
            causal[("ipc" if kind == TRACE_IPC else "io", value)].append({
                "stage": stages.get(subtype, f"stage_{subtype}"),
                "timestamp": event["timestamp"], "cpu": event["cpu"]})
        elif kind == TRACE_IRQ_CAUSE:
            causal[("io", value)].append({"stage": "irq",
                                           "vector": subtype,
                                           "timestamp": event["timestamp"],
                                           "cpu": event["cpu"]})
        elif kind == TRACE_EXEC:
            exec_rows.append({"pid": signed32(value),
                              "program": EXEC_NAMES.get(
                                  subtype, f"exec_kind_{subtype}"),
                              "timestamp": event["timestamp"],
                              "time_ms": (event["timestamp"] -
                                          snapshot["boot_tsc"]) * ms})

    cpu_rows, task_ticks = [], defaultdict(int)
    for cpu, rows in sorted(switches.items()):
        busy = idle = 0
        for index, row in enumerate(rows):
            end = rows[index + 1]["timestamp"] if index + 1 < len(rows) else snapshot["end_tsc"]
            duration = max(0, end - row["timestamp"])
            if row["next_idle"]:
                idle += duration
            else:
                busy += duration
                task_ticks[row["next"]] += duration
            trace.append({"name": f"pid_{row['next']}", "category": "sched",
                          "cpu": cpu, "start": row["timestamp"],
                          "duration": duration})
        total = busy + idle
        cpu_rows.append({"cpu": cpu, "busy_ms": busy * ms, "idle_ms": idle * ms,
                         "utilization_percent": busy * 100.0 / total if total else 0.0,
                         "switches": len(rows)})
    tasks = [{"pid": pid, "running_ms": ticks * ms}
             for pid, ticks in sorted(task_ticks.items(), key=lambda item: -item[1])]
    latencies = sorted(item["latency_ms"] for item in wake_latencies)
    wake_summary = {"count": len(latencies),
                    "p50_ms": percentile(latencies, 50),
                    "p95_ms": percentile(latencies, 95),
                    "max_ms": max(latencies) if latencies else 0.0,
                    "events": wake_latencies}
    irq_aggregate = defaultdict(lambda: {"count": 0, "total_ms": 0.0,
                                         "max_ms": 0.0})
    for row in irq_rows:
        item = irq_aggregate[(row["cpu"], row["vector"], row["owner"])]
        item["count"] += 1
        item["total_ms"] += row["duration_ms"]
        item["max_ms"] = max(item["max_ms"], row["duration_ms"])
    for key, values in irq_summaries.items():
        if not {"count", "total_cycles", "max_cycles"} <= values.keys():
            continue
        irq_aggregate[key] = {"count": values["count"],
                              "total_ms": values["total_cycles"] * ms,
                              "max_ms": values["max_cycles"] * ms}
    irqs = [{"cpu": key[0], "vector": key[1], "owner": key[2], **values}
            for key, values in irq_aggregate.items()]
    irqs.sort(key=lambda item: -item["total_ms"])
    causal_rows = []
    for key, stages in causal.items():
        start_stage = "send" if key[0] == "ipc" else "submit"
        terminal_stages = {"reply", "wake"} if key[0] == "ipc" else {"complete", "wake"}
        transactions, current = [], []
        for stage in stages:
            if stage["stage"] == start_stage and current:
                transactions.append(current)
                current = []
            current.append(stage)
        if current:
            transactions.append(current)
        for instance, transaction in enumerate(transactions):
            names = {stage["stage"] for stage in transaction}
            causal_rows.append({"kind": key[0], "cookie": key[1],
                                "instance": instance,
                                "complete": start_stage in names and
                                            bool(names & terminal_stages),
                                "stages": transaction})
    return {"cpus": cpu_rows, "top_tasks": tasks,
            "wake_latency": wake_summary, "execs": exec_rows}, irqs, causal_rows, trace


def percentile(values, percent):
    if not values:
        return 0.0
    return values[min(len(values) - 1, int((len(values) - 1) * percent / 100.0))]


def summarize_ahci_irq(counters, tsc_freq):
    values = counters.get("final", {}).get("ahci_irq", {})
    if not values:
        return {"completion_mode": "inline", "valid": False}
    components = ("ack", "lock_wait", "bookkeeping", "copy", "wake",
                  "next_submit", "unlock_exit")
    stages = components + ("locked_total", "handler_total")
    output = {"completion_mode": "inline", "valid": True}
    for stage in stages:
        prefix = f"{stage}."
        row = {key[len(prefix):]: value for key, value in values.items()
               if key.startswith(prefix)}
        count = row.get("count", 0)
        threshold = (count * 95 + 99) // 100
        cumulative = 0
        p95_cycles = 0
        for bucket in range(32):
            cumulative += row.get(f"hist_{bucket}", 0)
            if cumulative >= threshold and threshold:
                p95_cycles = 1 << bucket
                break
        output[stage] = {
            "count": count,
            "total_us": row.get("cycles", 0) * 1_000_000.0 / tsc_freq,
            "p95_us": p95_cycles * 1_000_000.0 / tsc_freq,
            "max_us": row.get("max", 0) * 1_000_000.0 / tsc_freq,
        }
    handler = output["handler_total"]
    stage_rows = [output[name] for name in components]
    if (any(row["count"] != handler["count"] for row in stage_rows) or
            abs(sum(row["total_us"] for row in stage_rows) -
                handler["total_us"]) > 0.001):
        output["valid"] = False
    output["spurious"] = {"count": values.get("spurious.count", 0),
                          "total_us": values.get("spurious.cycles", 0) *
                          1_000_000.0 / tsc_freq}
    output["orphan"] = {"count": values.get("orphan.count", 0),
                        "total_us": values.get("orphan.cycles", 0) *
                        1_000_000.0 / tsc_freq}
    for threshold in ("100us", "1ms"):
        prefix = f"long_tail_{threshold}."
        output[f"long_tail_{threshold}"] = {
            key[len(prefix):]: value for key, value in values.items()
            if key.startswith(prefix)}
    return output


def read_build_id(elf):
    result = subprocess.run(["readelf", "-n", str(elf)], check=True,
                            capture_output=True, text=True, timeout=10)
    match = re.search(r"Build ID:\s*([0-9a-fA-F]+)", result.stdout)
    if not match:
        raise PerfFormatError(f"kernel ELF has no build ID: {elf}")
    return match.group(1).lower()


def validate_symbol_elf(elf, manifest, expected):
    if not elf.is_file():
        raise PerfFormatError(f"kernel symbol ELF not found: {elf}")
    build_id = read_build_id(elf)
    if expected and build_id != expected:
        raise PerfFormatError(f"kernel symbol build ID mismatch: raw={expected} elf={build_id}")
    rows = [line.split("\t") for line in manifest.read_text(encoding="utf-8").splitlines()]
    if not any(len(row) >= 2 and row[0].lower() == build_id and row[1] == "myos.elf"
               for row in rows):
        raise PerfFormatError("kernel ELF build ID is absent from the symbol manifest")
    return build_id


def symbolize(chains, elf):
    kernel_addresses = sorted({address for chain in chains for address in chain["frames"]
                               if address >= KERNEL_BASE})
    resolved = {}
    if kernel_addresses:
        command = ["addr2line", "-f", "-C", "-e", str(elf)] + [hex(x) for x in kernel_addresses]
        result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
        lines = result.stdout.splitlines()
        if len(lines) != len(kernel_addresses) * 2:
            raise PerfFormatError("addr2line returned an unexpected number of rows")
        resolved = {address: (lines[index * 2], lines[index * 2 + 1])
                    for index, address in enumerate(kernel_addresses)}
    output, grouped = [], defaultdict(lambda: {"samples": 0, "addresses": set(), "location": "??:0"})
    for chain in chains:
        labels, locations = [], []
        for address in chain["frames"]:
            if address in resolved:
                function, location = resolved[address]
                label = function if function != "??" else f"0x{address:016x}"
            else:
                label, location = f"user@0x{address:016x}", "user mapping"
            labels.append(label)
            locations.append(location)
        item = dict(chain)
        item.update({"symbols": labels, "locations": locations})
        output.append(item)
        if chain["frames"][0] >= KERNEL_BASE:
            leaf = labels[0]
            grouped[leaf]["samples"] += chain["count"]
            grouped[leaf]["addresses"].add(f"0x{chain['frames'][0]:016x}")
            grouped[leaf]["location"] = locations[0]
    total = sum(item["samples"] for item in grouped.values())
    hotspots = [{"symbol": symbol, "samples": item["samples"],
                 "percent": item["samples"] * 100.0 / total if total else 0.0,
                 "location": item["location"], "addresses": sorted(item["addresses"])}
                for symbol, item in grouped.items()]
    hotspots.sort(key=lambda item: (-item["samples"], item["symbol"]))
    return hotspots, output


def write_trace(snapshot, events, output_dir):
    scale = 1_000_000.0 / snapshot["tsc_freq"]
    rows = [{"name": item["name"], "cat": item["category"], "ph": "X",
             "pid": 1, "tid": item["cpu"],
             "ts": (item["start"] - snapshot["boot_tsc"]) * scale,
             "dur": item["duration"] * scale} for item in events]
    (output_dir / "perf-trace.json").write_text(
        json.dumps({"traceEvents": rows, "displayTimeUnit": "ms"},
                   separators=(",", ":")) + "\n", encoding="utf-8")


def write_sample_views(chains, output_dir, source):
    folded, frames, frame_ids, samples, weights = [], [], {}, [], []
    for chain in chains:
        stack = list(reversed(chain["symbols"]))
        folded.append(";".join(name.replace(";", ":") for name in stack) +
                      f" {chain['count']}")
        indexes = []
        for name in stack:
            if name not in frame_ids:
                frame_ids[name] = len(frames)
                frames.append({"name": name})
            indexes.append(frame_ids[name])
        samples.append(indexes)
        weights.append(chain["count"])
    (output_dir / "perf.folded").write_text("\n".join(folded) + ("\n" if folded else ""), encoding="utf-8")
    profile = {"$schema": "https://www.speedscope.app/file-format-schema.json",
               "shared": {"frames": frames}, "activeProfileIndex": 0,
               "profiles": [{"type": "sampled", "name": f"XOS {source} callchains",
                              "unit": "none", "startValue": 0,
                              "endValue": sum(weights), "samples": samples,
                              "weights": weights}], "exporter": "xos perf-report.py"}
    (output_dir / "perf-speedscope.json").write_text(
        json.dumps(profile, separators=(",", ":")) + "\n", encoding="utf-8")


def analyze_counters(snapshot):
    snapshots = snapshot.get("counter_snapshots", [])
    if not snapshots:
        return {"final": {}}, [], [], {"cpus": [], "threshold_percent": 80}

    milestones = []
    for item in snapshots:
        milestones.append({"id": item["mark_id"], "name": item["name"],
                           "begin_tsc": item["begin_tsc"],
                           "end_tsc": item["end_tsc"],
                           "timestamp_ms": (item["begin_tsc"] - snapshot["boot_tsc"]) *
                                           1000.0 / snapshot["tsc_freq"],
                           "availability": item["availability"]})

    deltas = []
    gui = [item for item in snapshots if item["mark_id"] <= 5]
    for left, right in zip(gui, gui[1:]):
        row = {"from": left["name"], "to": right["name"],
               "duration_ms": (right["begin_tsc"] - left["begin_tsc"]) *
                              1000.0 / snapshot["tsc_freq"]}
        groups = defaultdict(dict)
        for name, end_value in right["values"].items():
            if name.startswith("unknown.") or name == "ahci.queue_depth":
                continue
            if name not in left["values"]:
                continue
            start_value = left["values"][name]
            if (name.endswith(".outstanding") or
                    name.endswith(".outstanding_peak")):
                group, field = name.split(".", 1)
                groups[group][f"{field}_start"] = start_value
                groups[group][f"{field}_end"] = end_value
                continue
            if end_value < start_value:
                raise PerfFormatError(f"counter rollback for {name} between "
                                      f"{left['name']} and {right['name']}")
            group, field = name.split(".", 1)
            groups[group][field] = end_value - start_value
        block = groups.get("block", {})
        for direction in ("read", "write"):
            commands = block.get(f"{direction}_commands", 0)
            sectors = block.get(f"{direction}_sectors", 0)
            block[f"{direction}_sectors_per_command"] = (
                sectors / commands if commands else None)
        row.update(groups)
        deltas.append(row)

    final_item = next((item for item in reversed(snapshots)
                       if item["mark_id"] == 6), snapshots[-1])
    final = defaultdict(dict)
    for name, value in final_item["values"].items():
        if name.startswith("unknown."):
            continue
        group, field = name.split(".", 1)
        final[group][field] = value
    block = final.get("block", {})
    accepted = block.get("submitted", 0)
    terminal = block.get("completed", 0) + block.get("failed", 0)
    if terminal > accepted:
        raise PerfFormatError("final block counters exceed accepted requests")
    block["outstanding"] = accepted - terminal
    for direction in ("read", "write"):
        commands = block.get(f"{direction}_commands", 0)
        sectors = block.get(f"{direction}_sectors", 0)
        block[f"{direction}_sectors_per_command"] = (
            sectors / commands if commands else None)

    readahead = final.get("readahead", {})
    for source in ("mmap", "read"):
        prefix = f"{source}."
        values = {key[len(prefix):]: value for key, value in readahead.items()
                  if key.startswith(prefix)}
        if not values:
            continue
        admitted = values.get("admitted_speculative", 0)
        hits = values.get("hits", 0)
        waste = (values.get("eviction_waste", 0) +
                 values.get("invalidation_waste", 0))
        outstanding = values.get("outstanding", 0)
        if admitted != hits + waste + outstanding:
            raise PerfFormatError(
                f"readahead {source} lifecycle conservation failed")
        resolved = hits + waste
        values["resolved_coverage"] = resolved / admitted if admitted else None
        values["resolved_waste_ratio"] = waste / resolved if resolved else None
        values["outstanding_ratio"] = outstanding / admitted if admitted else None
        readahead[source] = values
    for key in list(readahead):
        if key.startswith("mmap.") or key.startswith("read."):
            del readahead[key]

    cpus = []
    event = final.get("event", {})
    for cpu in range(4):
        capacity = event.get(f"cpu{cpu}.capacity")
        if capacity is None:
            continue
        high = event.get(f"cpu{cpu}.high_water", 0)
        cpus.append({"cpu": cpu, "capacity": capacity,
                     "attempted": event.get(f"cpu{cpu}.attempted", 0),
                     "committed": event.get(f"cpu{cpu}.committed", 0),
                     "high_water": high,
                     "utilization_percent": high * 100.0 / capacity if capacity else 0,
                     "degraded": bool(capacity and high * 100 >= capacity * 80)})
    counter = final.get("counter", {})
    counter_overflow = bool(counter.get("overflow", 0) or any(
        item["availability"] & (1 << 63) for item in snapshots))
    return {"final": dict(final),
            "counter_capacity": counter.get("capacity"),
            "counter_count": counter.get("count"),
            "counter_overflow": counter_overflow}, milestones, deltas, {
        "cpus": cpus, "threshold_percent": 80}


def write_outputs(snapshot, phases, tests, errors, trace, hotspots, chains,
                  scheduling, irqs, causal, output_dir, metadata, kernel_elf,
                  build_id, ra_max_pages, run_manifest):
    output_dir.mkdir(parents=True, exist_ok=True)
    duration_ms = (snapshot["end_tsc"] - snapshot["boot_tsc"]) * 1000.0 / snapshot["tsc_freq"]
    sources = {chain["source"] for chain in chains}
    source_id = metadata.get("sampling_source", 0) if metadata else 0
    source = ("mixed" if len(sources) > 1 else
              SOURCE_NAMES.get(next(iter(sources)), "unknown") if sources else
              SOURCE_NAMES.get(source_id, "unknown"))
    pmu = source == "pmu_nmi"
    per_cpu_samples = defaultdict(int)
    for chain in chains:
        per_cpu_samples[chain["cpu"]] += chain["count"]
    sampling = {"source": source, "frequency_hz_per_cpu": 1000 if pmu else 100,
                "confidence": "high" if pmu else "degraded",
                "interrupts_disabled_visible": pmu,
                "sample_hits": snapshot["sample_hits"],
                "lost_samples": snapshot["lost_samples"],
                "samples_per_cpu": dict(sorted(per_cpu_samples.items())),
                "nmi_count": metadata.get("nmi_count", 0) if metadata else 0,
                "handler_cycles": metadata.get("handler_cycles", 0) if metadata else 0,
                "truncated_callchains": metadata.get("truncated_callchains", 0) if metadata else 0,
                "pmu_active_mask": metadata.get("pmu_active_mask", 0) if metadata else 0}
    counters, milestones, phase_deltas, event_buffers = analyze_counters(snapshot)
    ahci_irq_stages = summarize_ahci_irq(counters, snapshot["tsc_freq"])
    trace_valid = ((metadata.get("trace_lost", 0) if metadata else 0) == 0 and
                   snapshot["lost_samples"] == 0 and
                   not any(cpu["degraded"] for cpu in event_buffers["cpus"]))
    counter_overflow = counters.get("counter_overflow", False)
    summary = {"format": "xos-perf-summary-v3",
               "complete": snapshot["complete"] and not counter_overflow,
               "end_reason": snapshot["end_reason"], "duration_ms": duration_ms,
               "record_count": snapshot["record_count"], "trace_errors": errors,
               "trace_lost": metadata.get("trace_lost", 0) if metadata else 0,
               "phases": phases, "tests": tests, "sampling": sampling,
               "kernel_symbols": {"elf": str(kernel_elf), "build_id": build_id},
               "top_kernel_hotspots": hotspots, "scheduling": scheduling,
               "irqs": irqs, "causal_chains": causal,
               "ahci_irq_stages": ahci_irq_stages,
               "event_buffers": event_buffers, "counters": counters,
               "gui_milestones": milestones, "gui_phase_deltas": phase_deltas}
    summary["configuration"] = {
        "ra_max_pages": ra_max_pages,
        "build_id": build_id,
        "git_sha": run_manifest.get("git_sha") if run_manifest else None,
        "source_digest": run_manifest.get("source_digest") if run_manifest else None,
    }
    if metadata:
        for field in ("abi_version", "boot_tsc", "tsc_freq", "record_count"):
            if field in metadata and metadata[field] != snapshot.get(field):
                raise PerfFormatError(f"metadata mismatch for {field}")
    (output_dir / "perf-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    lines = ["XOS performance report", f"complete: {str(summary['complete']).lower()}",
             f"end reason: {snapshot['end_reason']}", f"total wall time: {duration_ms:.3f} ms",
             f"records: {snapshot['record_count']}", f"trace errors: {len(errors)}", "",
             f"Kernel hotspots ({source}, {sampling['confidence']} confidence):",
             f"  samples={snapshot['sample_hits']} lost={snapshot['lost_samples']} "
             f"nmi={sampling['nmi_count']} build-id={build_id}"]
    lines.extend(f"  {x['percent']:6.2f}% {x['samples']:8d}  {x['symbol']}  ({x['location']})"
                 for x in hotspots[:30])
    if not hotspots:
        lines.append("  (none)")
    lines.extend(["", "CPU scheduling:" if trace_valid else
                  "CPU scheduling: INVALID (trace loss)"])
    lines.extend(f"  cpu{x['cpu']}: busy={x['busy_ms']:.3f}ms idle={x['idle_ms']:.3f}ms "
                 f"util={x['utilization_percent']:.1f}% switches={x['switches']}"
                 for x in scheduling["cpus"])
    wake = scheduling["wake_latency"]
    if trace_valid:
        lines.append(f"  wake latency: n={wake['count']} p50={wake['p50_ms']:.3f}ms "
                     f"p95={wake['p95_ms']:.3f}ms max={wake['max_ms']:.3f}ms")
    else:
        lines.append("  wake latency: INVALID")
    lines.extend(["", "Clang job exec markers:"])
    lines.extend(f"  {x['time_ms']:10.3f} ms  pid={x['pid']:<5} {x['program']}"
                 for x in scheduling["execs"])
    if not scheduling["execs"]:
        lines.append("  (none)")
    if phase_deltas:
        lines.extend(["", "GUI milestone deltas:"])
        for delta in phase_deltas:
            block, fat, ra, ahci = (delta.get("block", {}), delta.get("fat", {}),
                                    delta.get("readahead", {}), delta.get("ahci", {}))
            lines.append(
                f"  {delta['from']} -> {delta['to']}: {delta['duration_ms']:.3f}ms "
                f"read={block.get('read_sectors', 0)}sec/{block.get('read_commands', 0)}cmd "
                f"fat_steps={fat.get('demand_steps', 0) + fat.get('readahead_steps', 0)} "
                f"restarts={fat.get('demand_head_restarts', 0) + fat.get('readahead_head_restarts', 0)} "
                f"ra_hit/waste={ra.get('hits', 0)}/{ra.get('waste', 0)} "
                f"queue/service={ahci.get('queue_wait_cycles', 0)}/"
                f"{ahci.get('service_cycles', 0)}cy wake_xcpu={ahci.get('cross_cpu_wakes', 0)}")
    lines.extend(["", "Business IRQ time:"])
    lines.extend(f"  cpu{x['cpu']} vector=0x{x['vector']:02x} owner={x['owner']} "
                 f"count={x['count']} total={x['total_ms']:.3f}ms "
                 f"max={x['max_ms']:.3f}ms" for x in irqs[:20])
    if not irqs:
        lines.append("  (none)")
    lines.extend(["", f"Causal chains: {len(causal)}", "",
                  "Startup phases (inclusive / exclusive):"])
    lines.extend(f"  {x['name']:<28} {x['inclusive_ms']:10.3f} / {x['exclusive_ms']:10.3f} ms"
                 for x in phases)
    lines.extend(["", "Test markers:"])
    lines.extend(f"  test #{x['id']:<3} {x['name']:<24} {x['status']:<7} "
                 f"{x['duration_ms']:10.3f} ms" for x in tests)
    if errors:
        lines.extend(["", "Trace errors:"] + [f"  {error}" for error in errors])
    (output_dir / "perf-report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    write_trace(snapshot, trace, output_dir)
    write_sample_views(chains, output_dir, source)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("raw", type=Path)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("build/perf-results"))
    parser.add_argument("--kernel-elf", type=Path,
                        default=root / "build/perf-symbols/myos.elf")
    parser.add_argument("--build-id-manifest", type=Path,
                        default=root / "build/perf-symbols/build-id-manifest.tsv")
    parser.add_argument("--test-manifest", type=Path,
                        default=root / "build/perf-symbols/test-manifest.tsv")
    parser.add_argument("--ra-max-pages", choices=("off", "0", "4", "8", "16"),
                        default="16")
    parser.add_argument("--run-manifest", type=Path)
    args = parser.parse_args()
    try:
        snapshot = parse_raw(args.raw)
        metadata = json.loads(args.metadata.read_text(encoding="utf-8")) if args.metadata else None
        run_manifest = (json.loads(args.run_manifest.read_text(encoding="utf-8"))
                        if args.run_manifest else None)
        test_names = read_test_manifest(args.test_manifest)
        phases, tests, errors, phase_trace = analyze_phases(snapshot, test_names)
        scheduling, irqs, causal, event_trace = analyze_events(snapshot)
        build_id = validate_symbol_elf(args.kernel_elf, args.build_id_manifest,
                                       snapshot["kernel_build_id"])
        hotspots, chains = symbolize(snapshot["chains"], args.kernel_elf)
        write_outputs(snapshot, phases, tests, errors, phase_trace + event_trace,
                      hotspots, chains, scheduling, irqs, causal,
                      args.output_dir, metadata, args.kernel_elf, build_id,
                      0 if args.ra_max_pages == "off" else int(args.ra_max_pages),
                      run_manifest)
    except (OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError,
            PerfFormatError) as error:
        print(f"perf-report: {error}", file=sys.stderr)
        return 1
    print(f"perf-report: wrote {args.output_dir / 'perf-report.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
