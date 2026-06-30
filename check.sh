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
    "-D__attribute__(x)="
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
#define __builtin_unreachable() ((void)0)
EOF

SPARSE_FLAGS+=(-include "$SPARSE_COMPAT")

echo "Running sparse on kernel sources..."

# Collect all kernel .c files (exclude user-space code)
KERNEL_SOURCES=()
for f in kernel/xcore/*.c arch/x64/*.c kernel/xcore/mem/*.c kernel/bsd/*.c kernel/driver/*.c; do
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

# ===================== Step 1.5: #include layer check =====================
echo ""
echo "=== Step 1.5: #include layer check ==="

INCLUDE_FAIL=0

# kernel/xcore/ must not include kernel/bsd/ or kernel/driver/ headers
echo "Checking kernel/xcore/ #include violations..."
if grep -rn '#include "kernel/bsd/' kernel/xcore/ 2>/dev/null; then
    echo "FAIL: xcore includes bsd"
    INCLUDE_FAIL=1
fi
if grep -rn '#include "kernel/driver/' kernel/xcore/ 2>/dev/null; then
    echo "FAIL: xcore includes driver"
    INCLUDE_FAIL=1
fi

# kernel/driver/ must not include kernel/bsd/ headers (devtmpfs excepted)
echo "Checking kernel/driver/ #include violations..."
if grep -rn '#include "kernel/bsd/' kernel/driver/ 2>/dev/null | grep -v devtmpfs; then
    echo "FAIL: driver includes bsd (except devtmpfs)"
    INCLUDE_FAIL=1
fi

# Verify kernel/xcore/mm_types.h does not pull in bsd/driver
echo "Checking kernel/xcore/mm_types.h is self-contained..."
if grep -q '#include "kernel/bsd/' kernel/xcore/mm_types.h 2>/dev/null; then
    echo "FAIL: mm_types.h includes bsd"
    INCLUDE_FAIL=1
fi
if grep -q '#include "kernel/driver/' kernel/xcore/mm_types.h 2>/dev/null; then
    echo "FAIL: mm_types.h includes driver"
    INCLUDE_FAIL=1
fi

if [ "$INCLUDE_FAIL" -ne 0 ]; then
    echo "#include layer check failed."
    exit 1
fi
echo "#include layer check passed."

# ===================== Step 2: Sanitizer check (skipped) =====================
echo ""
echo "=== Step 2: Sanitizer build + boot test ==="
echo "Skipped (not yet ready)."
