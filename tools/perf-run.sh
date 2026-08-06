#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
result_dir=${1:-"$root/build/perf-results"}
serial_log="$root/log.txt"
host_log="$result_dir/qemu-host.log"
stall_limit=60

mkdir -p "$result_dir"
cd "$root"
./build.sh --perf
./run.sh >"$host_log" 2>&1 &
qemu_pid=$!

cleanup() {
    if kill -0 "$qemu_pid" 2>/dev/null; then
        kill "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

last_size=-1
stalled=0
while kill -0 "$qemu_pid" 2>/dev/null; do
    current_size=0
    if [ -f "$serial_log" ]; then
        current_size=$(stat -c %s "$serial_log" 2>/dev/null || true)
        current_size=${current_size:-0}
        if rg -q 'perf: exported .* complete=1' "$serial_log"; then
            break
        fi
    fi
    if [ "$current_size" != "$last_size" ]; then
        last_size=$current_size
        stalled=0
    else
        stalled=$((stalled + 1))
    fi
    if [ "$stalled" -ge "$stall_limit" ]; then
        echo "perf-run: serial stalled; recovering the latest durable checkpoint" >&2
        break
    fi
    sleep 1
done

cleanup
trap - EXIT INT TERM
tools/perf-extract.sh build/disk.img "$result_dir"
