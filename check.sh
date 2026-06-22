#!/bin/bash
# Sparse static analysis for kernel code
# Usage: ./check.sh

set -e

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
    rm -f "$SPARSE_COMPAT"
    exit 1
fi

# Run sparse per-file to avoid __bitwise type state leakage across translation units
FAILED=0
WARNFILE=/tmp/sparse-output.txt
> "$WARNFILE"
for f in "${KERNEL_SOURCES[@]}"; do
    sparse "${SPARSE_FLAGS[@]}" "$f" 2>&1 \
        | grep -v "non-ANSI function declaration" \
        | grep -v "^sparse: " \
        >> "$WARNFILE" || true
done

rm -f "$SPARSE_COMPAT"

if [ -s "$WARNFILE" ]; then
    cat "$WARNFILE"
    echo ""
    echo "Sparse check failed with $(wc -l < "$WARNFILE") warning(s)."
    exit 1
fi
echo "Sparse check passed."
