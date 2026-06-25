#!/bin/bash
# Sparse static analysis + sanitizer boot test for kernel code
# Usage: ./check.sh

SPARSE_COMPAT=""
WARNFILE=/tmp/sparse-output.txt

cleanup() {
    rm -f "$SPARSE_COMPAT" "$WARNFILE"
    # Kill QEMU process group if still running
    if [ -n "$QEMU_PGID" ]; then
        kill -- -"$QEMU_PGID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# ===================== Step 1: Sparse check =====================

# Check if sparse is available
if ! command -v sparse &> /dev/null; then
    echo "Error: sparse is not installed. Install with: sudo apt install sparse"
    exit 1
fi

SPARSE_FLAGS=(
    -std=gnu11
    -m64
    -D__KERNEL__
    -D__CHECKER__
    -D__ATOMIC_ACQUIRE=2
    -D__ATOMIC_RELEASE=3
    -D__ATOMIC_RELAXED=0
    -D__ATOMIC_SEQ_CST=5
    -Waddress-space
    -Wdecl
    -Wdo-while
    -Wtransparent-union
    -Wreturn-void-ptr
    -I.
)

# Create a temporary sparse compat header for GCC builtins sparse doesn't understand
SPARSE_COMPAT=$(mktemp /tmp/sparse-compat-XXXXXX.h)
cat > "$SPARSE_COMPAT" << 'EOF'
/* Sparse compatibility: stub out GCC builtins that sparse doesn't understand */
/* Prevent GCC's stdarg.h from being included (sparse can't parse __builtin_va_*) */
#ifndef _STDARG_H
#define _STDARG_H
typedef struct { char __dummy; } __gnuc_va_list;
typedef __gnuc_va_list va_list;
#define va_start(v, l) ((void)0)
#define va_end(v) ((void)0)
#define va_arg(v, t) ((t)0)
#endif
#define __atomic_exchange_n(p, val, ord) (*(p))
#define __atomic_store_n(p, val, ord) (*(p) = (val))
#define __atomic_load_n(p, ord) (*(p))
#define __atomic_add_fetch(p, val, ord) (*(p) += (val))
#define __atomic_sub_fetch(p, val, ord) (*(p) -= (val))
#define __atomic_fetch_sub(p, val, ord) (*(p) -= (val))
#define __atomic_or_fetch(p, val, ord) (*(p) |= (val))
#define __atomic_and_fetch(p, val, ord) (*(p) &= (val))
#define __sync_val_compare_and_swap(p, old, new) (*(p))
EOF

SPARSE_FLAGS+=(-include "$SPARSE_COMPAT")

echo "Running sparse on kernel sources..."

# Collect all kernel .c files (exclude user-space code)
KERNEL_SOURCES=()
for f in kernel/*.c arch/x64/*.c kernel/mem/*.c; do
    [ -f "$f" ] && KERNEL_SOURCES+=("$f")
done

if [ ${#KERNEL_SOURCES[@]} -eq 0 ]; then
    echo "Error: No kernel source files found"
    exit 1
fi

# Run sparse per-file to avoid __bitwise type state leakage across translation units
> "$WARNFILE"
for f in "${KERNEL_SOURCES[@]}"; do
    sparse "${SPARSE_FLAGS[@]}" "$f" 2>&1 \
        | grep -v "non-ANSI function declaration" \
        | grep -v "^sparse: " \
        >> "$WARNFILE" || true
done

# Clean up sparse compat header early (no longer needed)
rm -f "$SPARSE_COMPAT"
SPARSE_COMPAT=""

if [ -s "$WARNFILE" ]; then
    cat "$WARNFILE"
    echo ""
    echo "Sparse check failed with $(wc -l < "$WARNFILE") warning(s)."
    exit 1
fi
echo "Sparse check passed."

# ===================== Step 2: Sanitizer check =====================
echo ""
echo "=== Step 2: Sanitizer build + boot test ==="

./build.sh --sanitizer

# Start QEMU in a new process group so we can kill the whole group
rm -f log.txt
setsid timeout 30 ./run.sh &
QEMU_PID=$!
QEMU_PGID=$(ps -o pgid= -p "$QEMU_PID" | tr -d ' ')

# Wait for QEMU to boot and stabilize
sleep 20

# Kill entire QEMU process group
kill -- -"$QEMU_PGID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PGID=""

# Check log.txt for KASAN/KCSAN reports
if [ ! -f log.txt ]; then
    echo "Error: log.txt not found (QEMU may have failed to start)"
    exit 1
fi

if grep -q "KASAN:" log.txt || grep -q "KCSAN:" log.txt; then
    echo "Sanitizer detected issues:"
    grep -E "KASAN:|KCSAN:" log.txt
    exit 1
fi
echo "Sanitizer check passed: no issues detected during boot."
