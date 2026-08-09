#!/bin/bash
set -euo pipefail

image=${1:-build/disk.img}
result_dir=${2:-build/perf-results}

if [ ! -f "$image" ]; then
    echo "perf-extract: image not found: $image" >&2
    exit 1
fi

partition=$(python3 - "$image" <<'PY'
import os
import struct
import sys

path = sys.argv[1]
size = os.path.getsize(path)
with open(path, "rb") as stream:
    mbr = stream.read(512)
if len(mbr) != 512 or mbr[510:512] != b"\x55\xaa":
    raise SystemExit("perf-extract: invalid MBR")

matches = []
for index in range(4):
    entry = mbr[446 + index * 16:462 + index * 16]
    part_type = entry[4]
    start, sectors = struct.unpack_from("<II", entry, 8)
    if part_type in (0x0b, 0x0c) and sectors:
        end = (start + sectors) * 512
        if start == 0 or end > size or end < start * 512:
            raise SystemExit("perf-extract: FAT32 partition exceeds image bounds")
        matches.append((start, sectors))
if len(matches) != 1:
    raise SystemExit(f"perf-extract: expected one FAT32 partition, found {len(matches)}")
print(*matches[0])
PY
)
read -r part_start part_sectors <<<"$partition"

tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$tmp_dir"' EXIT
part_image="$tmp_dir/root.img"
dd if="$image" of="$part_image" bs=512 skip="$part_start" \
    count="$part_sectors" status=none
if ! fsck.fat -n "$part_image" >/dev/null; then
    echo "perf-extract: FAT checker reported guest-created metadata issues; continuing read-only" >&2
fi

mkdir -p "$result_dir"
if ! mcopy -i "$part_image" ::var/perf/perf.raw "$result_dir/perf.raw" 2>/dev/null; then
    echo "perf-extract: final perf.raw missing, trying recoverable .tmp" >&2
    if ! mcopy -i "$part_image" ::var/perf/PERF.TMP \
            "$result_dir/perf.raw" 2>/dev/null; then
        # Compatibility with images created before temporary names became 8.3.
        mcopy -i "$part_image" ::var/perf/perf.raw.tmp "$result_dir/perf.raw"
    fi
fi
if ! mcopy -i "$part_image" ::var/perf/metadata.json \
        "$result_dir/metadata.json" 2>/dev/null; then
    # The current guest FAT driver stores only 8.3 names and truncates .json.
    if ! mcopy -i "$part_image" ::var/perf/METADATA.JSO \
            "$result_dir/metadata.json" 2>/dev/null; then
        echo "perf-extract: final metadata missing, trying checkpoint metadata" >&2
        mcopy -i "$part_image" ::var/perf/meta.tmp \
            "$result_dir/metadata.json"
    fi
fi

python3 "$(dirname "$0")/perf-report.py" \
    "$result_dir/perf.raw" --metadata "$result_dir/metadata.json" \
    --output-dir "$result_dir"
