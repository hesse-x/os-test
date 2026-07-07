#!/bin/bash
# sparse_check.sh — Sparse static analysis + #include layer check for kernel code
# Standalone runnable: ./build_script/sparse_check.sh [base|--all]
# Returns 0 = passed, 1 = sparse/layer violations or missing environment
#
# Scope (incremental by default, sparse static-analysis part only):
#   (no arg)         only kernel .c changed vs origin/master
#   origin/<branch>  only those changed vs origin/<branch>
#   <branch>         only those changed vs local <branch>
#   --all            all kernel .c (pre-change behavior)
# Empty incremental result → sparse step "nothing changed, skipped" (exit 0 for
# that step). The #include layer check below is a directory-level invariant grep
# and stays FULL in every mode (it's cheap and not file-diff-shaped).
#
# Invoked by check.sh --filter sparse.

# resolve repo root (this script lives in build_script/)
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=_diff_files.sh
source "$SCRIPT_DIR/_diff_files.sh" "${1:-}"

SPARSE_COMPAT=""
WARNFILE=$(mktemp /tmp/sparse-output.XXXXXX)

cleanup() {
    rm -f "$SPARSE_COMPAT" "$WARNFILE"
}
trap cleanup EXIT INT TERM

# ===================== Step 1: Sparse check =====================

if ! command -v sparse &> /dev/null; then
    echo "Error: sparse is not installed. Install with: sudo apt install sparse"
    exit 1
fi

# Flags align with current compile behavior (CMakeLists.txt global C flags + include paths):
#   -std=gnu17, -I. + -Iinclude/uapi (xos/* UAPI headers)
# Note: do NOT add -nostdinc -isystem <gcc freestanding inc>. gcc compilation uses this pair to isolate
# host glibc headers, but sparse has incomplete support for the -nostdinc + -isystem #include_next chain,
# which causes gcc's bundled stdint.h to fail self-referential resolution. sparse can use its default freestanding headers,
# only -Iinclude/uapi is needed so "xos/*.h" UAPI headers can resolve (otherwise "unable to open 'xos/...'").
SPARSE_FLAGS=(
    -std=gnu17
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
    -Iinclude/uapi
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

if [ "$CHECK_MODE" = "incremental" ]; then
    echo "Incremental sparse: base = $BASE (layer check below stays full)"
fi

# Collect all kernel .c files (exclude user-space code), then narrow to the diff
# set in incremental mode.
ALL_KERNEL_SOURCES=()
for f in kernel/xcore/*.c arch/x64/*.c kernel/xcore/mem/*.c kernel/bsd/*.c kernel/driver/*.c; do
    [ -f "$f" ] && ALL_KERNEL_SOURCES+=("$f")
done

if [ ${#ALL_KERNEL_SOURCES[@]} -eq 0 ]; then
    echo "Error: No kernel source files found"
    exit 1
fi

if [ "$CHECK_MODE" = "incremental" ]; then
    # diff paths are repo-root-relative; ALL_KERNEL_SOURCES entries already are
    # (cd to ROOT_DIR happened in _diff_files.sh).
    declare -A DIFF_SET
    while IFS= read -r df; do
        [ -n "$df" ] && DIFF_SET["$df"]=1
    done <<< "$(filter_changed '*.c')"
    KERNEL_SOURCES=()
    for f in "${ALL_KERNEL_SOURCES[@]}"; do
        [ -n "${DIFF_SET[$f]}" ] && KERNEL_SOURCES+=("$f")
    done
    if [ ${#KERNEL_SOURCES[@]} -eq 0 ]; then
        echo "No changed kernel .c files vs $BASE — sparse step skipped."
        SPARSE_SKIPPED=1
    else
        SPARSE_SKIPPED=0
    fi
else
    KERNEL_SOURCES=("${ALL_KERNEL_SOURCES[@]}")
    SPARSE_SKIPPED=0
fi

# Run sparse per-file to avoid __bitwise type state leakage across translation units
if [ "$SPARSE_SKIPPED" -ne 1 ]; then
    > "$WARNFILE"
    for f in "${KERNEL_SOURCES[@]}"; do
        sparse "${SPARSE_FLAGS[@]}" "$f" 2>&1 \
            | grep -v "non-ANSI function declaration" \
            | grep -v "^sparse: " \
            | grep -v "warning: preprocessor token __.* redefined" \
            | grep -v "this was the original definition" \
            | grep -v "note: in included file" \
            >> "$WARNFILE" || true
    done
fi

# Clean up sparse compat header early (no longer needed)
rm -f "$SPARSE_COMPAT"
SPARSE_COMPAT=""

if [ -s "$WARNFILE" ]; then
    cat "$WARNFILE"
    echo ""
    echo "Sparse check failed with $(wc -l < "$WARNFILE") warning(s)."
    exit 1
fi
if [ "$SPARSE_SKIPPED" -ne 1 ]; then
    echo "Sparse check passed."
fi

# ===================== Step 1.5: #include layer check =====================
# Always full (every mode): a directory-level invariant grep, not file-shaped.
echo ""
echo "=== #include layer check (full) ==="

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
