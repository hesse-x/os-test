#!/usr/bin/env python3
"""Synthetic raw ABI tests for perf-report.py."""

import importlib.util
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("perf-report.py")
SPEC = importlib.util.spec_from_file_location("perf_report", MODULE_PATH)
PERF = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PERF)


def record(payload, sequence, ident, kind, aux, value=0):
    return struct.pack("<QIHBBII", payload, sequence, ident, kind, aux, 24,
                       value)


def make_raw(minor=3, counters=True, duplicate=False, omit_end=False,
             counter_ident=1):
    boot, end, frequency = 1000, 100000, 1000000
    records = []
    if counters:
        sequence = 0
        for snapshot in range(1, 7):
            records.append(record(boot + snapshot * 100, sequence, snapshot,
                                  9, snapshot, 1))
            sequence += 1
            records.append(record(0x1f, sequence, 0, 10, snapshot))
            sequence += 1
            records.append(record(snapshot, sequence, counter_ident, 11,
                                  snapshot))
            sequence += 1
            if duplicate and snapshot == 1:
                records.append(record(snapshot, sequence, 1, 11, snapshot))
                sequence += 1
            if not (omit_end and snapshot == 6):
                records.append(record(boot + snapshot * 100 + 10, sequence,
                                      snapshot, 12, snapshot))
                sequence += 1
    blob = b"".join(records)
    header = bytearray(88)
    header[:8] = b"XOSPERF\0"
    struct.pack_into("<HHBBHII", header, 8, 1, minor, 1, 64, 88, 1, 1)
    header[24:40] = bytes(range(1, 17))
    struct.pack_into("<QQQQ", header, 40, boot, frequency, boot, end)
    struct.pack_into("<Q", header, 72, len(blob))
    struct.pack_into("<I", header, 80, zlib.crc32(header))
    footer = bytearray(64)
    footer[:8] = b"XOSEND\0\0"
    struct.pack_into("<QQIIQQ", footer, 8, len(blob), len(records), 1, 1, 0,
                     0)
    struct.pack_into("<II", footer, 48, zlib.crc32(blob), 64)
    footer[56:60] = bytes(range(17, 21))
    return bytes(header) + blob + bytes(footer)


class PerfReportRawTest(unittest.TestCase):
    def parse(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "perf.raw"
            path.write_bytes(data)
            return PERF.parse_raw(path)

    def test_valid_abi_13_counters(self):
        snapshot = self.parse(make_raw())
        self.assertEqual(snapshot["raw_minor"], 3)
        self.assertEqual(len(snapshot["counter_snapshots"]), 6)
        self.assertEqual(snapshot["counter_snapshots"][-1]["name"], "final")

    def test_old_abi_12_remains_supported(self):
        snapshot = self.parse(make_raw(minor=2, counters=False))
        self.assertEqual(snapshot["counter_snapshots"], [])

    def test_unknown_counter_is_preserved(self):
        snapshot = self.parse(make_raw(counter_ident=60000))
        self.assertEqual(snapshot["counter_snapshots"][0]["values"],
                         {"unknown.60000": 1})

    def test_duplicate_counter_is_rejected(self):
        with self.assertRaisesRegex(PERF.PerfFormatError, "counter value"):
            self.parse(make_raw(duplicate=True))

    def test_missing_counter_end_is_rejected(self):
        with self.assertRaisesRegex(PERF.PerfFormatError, "incomplete"):
            self.parse(make_raw(omit_end=True))

    def test_counter_rollback_is_rejected(self):
        snapshot = self.parse(make_raw())
        snapshot["counter_snapshots"][1]["values"]["block.submitted"] = 0
        with self.assertRaisesRegex(PERF.PerfFormatError, "rollback"):
            PERF.analyze_counters(snapshot)


if __name__ == "__main__":
    unittest.main()
