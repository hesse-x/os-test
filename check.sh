#!/bin/bash
# check.sh — check scheduler
#
# Usage:
#   ./check.sh                          # run all four (in order), incremental vs origin/master
#   ./check.sh --filter sparse,iwyu     # run only specified items, in given order
#   ./check.sh --filter iwyu            # single item
#   ./check.sh origin/perf              # incremental vs origin/perf
#   ./check.sh plan                     # incremental vs local branch plan
#   ./check.sh --all                    # full scan (all sources)
#   ./check.sh --filter iwyu origin/perf  # filter + base
#
# Valid check items: sparse, iwyu, clang-format, clang-tidy
#   clang-tidy is a placeholder for now (see doc/design/code_standard.md).
#
# Base argument (optional, positional): forwarded to each sub-script.
#   (none)         sub-scripts default to incremental vs origin/master
#   --all          full scan
#   <ref>          incremental vs <ref> (origin/<branch> or local <branch>)
#
# Exit code: all pass 0; any failure 1; argument error 2.

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# Check item → sub-script mapping
declare -A CHECK_SCRIPT=(
    [sparse]="build_script/sparse_check.sh"
    [iwyu]="build_script/iwyu_check.sh"
    [clang-format]="build_script/clang_format_check.sh"
    [clang-tidy]="build_script/clang_tidy_check.sh"
)
ALL_CHECKS=(sparse iwyu clang-format clang-tidy)

# ===================== Parse --filter and base arg =====================
FILTER=""
FILTER_SET=0
BASE_ARG=""
BASE_SET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --filter)
            FILTER="$2"
            FILTER_SET=1
            shift 2
            ;;
        --filter=*)
            FILTER="${1#--filter=}"
            FILTER_SET=1
            shift
            ;;
        -h|--help)
            sed -n '2,18p' "$0"
            exit 0
            ;;
        --all)
            BASE_ARG="--all"
            BASE_SET=1
            shift
            ;;
        -*)
            echo "Unknown option: $1"
            echo "Usage: $0 [--filter sparse,iwyu,...] [--all | <ref>]"
            exit 2
            ;;
        *)
            # A base/ref argument. Only one is allowed.
            if [ "$BASE_SET" -eq 1 ]; then
                echo "Error: multiple base arguments ('$BASE_ARG' and '$1')."
                echo "Usage: $0 [--filter sparse,iwyu,...] [--all | <ref>]"
                exit 2
            fi
            BASE_ARG="$1"
            BASE_SET=1
            shift
            ;;
    esac
done

# Expand filter into an ordered list; if --filter not passed, run all
if [ "$FILTER_SET" -eq 0 ]; then
    CHECKS=("${ALL_CHECKS[@]}")
else
    if [ -z "$FILTER" ]; then
        echo "Error: --filter value is empty. Valid: ${ALL_CHECKS[*]}"
        exit 2
    fi
    CHECKS=()
    IFS=',' read -ra _parts <<< "$FILTER"
    for c in "${_parts[@]}"; do
        if [ -z "$c" ]; then
            echo "Error: empty check name in --filter '$FILTER'. Valid: ${ALL_CHECKS[*]}"
            exit 2
        fi
        if [ -z "${CHECK_SCRIPT[$c]}" ]; then
            echo "Error: unknown check '$c'. Valid: ${ALL_CHECKS[*]}"
            exit 2
        fi
        CHECKS+=("$c")
    done
fi

# ===================== Sequential execution =====================
FAIL=0
STEP=0
for c in "${CHECKS[@]}"; do
    STEP=$((STEP + 1))
    echo ""
    echo "=== Step $STEP: $c ==="
    bash "$SCRIPT_DIR/${CHECK_SCRIPT[$c]}" $BASE_ARG
    rc=$?
    if [ $rc -ne 0 ]; then
        FAIL=1
        echo "Step $STEP ($c) FAILED (exit $rc)."
    fi
done

# ===================== Summary =====================
echo ""
if [ "$FAIL" -ne 0 ]; then
    echo "check.sh: FAILED (one or more checks did not pass)."
    exit 1
fi
echo "check.sh: all selected checks passed."
exit 0
