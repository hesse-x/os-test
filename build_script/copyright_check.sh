#!/bin/bash
# copyright_check.sh — copyright header gate
# Standalone runnable: ./build_script/copyright_check.sh [base|--all]
# Returns 0 = no violations, 1 = missing/malformed copyright headers
#
# Coverage: all .c/.cc/.h/.S in the repo,
#           excluding third_party/, build/, build_script/.
# Strictness: each file must start with a block comment containing both
#   "Copyright (c) <year> hesse" and "SPDX-License-Identifier: MIT"
#   within the first 5 lines.
#
# Scope (incremental by default):
#   (no arg)         only code files changed vs origin/master
#   origin/<branch>  only those changed vs origin/<branch>
#   <branch>         only those changed vs local <branch>
#   --all            all code files (pre-change behavior)
#
# Invoked by check.sh --filter copyright.

EXPECTED_COPYRIGHT="Copyright (c)"
EXPECTED_SPDX="SPDX-License-Identifier: MIT"
MAX_HEADER_LINES=5

# resolve repo root (this script lives in build_script/)
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=_diff_files.sh
source "$SCRIPT_DIR/_diff_files.sh" "${1:-}"

# ===================== Collect candidate files =====================
# All code files (.c/.cc/.h/.S), excluding third_party/ and build/.

collect_all_files() {
    find . -type f \( -name "*.c" -o -name "*.cc" -o -name "*.h" -o -name "*.S" \) \
        -not -path './build/*' \
        -not -path './third_party/*' \
        -not -path './build_script/*' \
        -print0 2>/dev/null | sort -z | tr '\0' '\n'
}

if [ "$CHECK_MODE" = "incremental" ]; then
    CANDIDATES=$(filter_changed "*.c" "*.cc" "*.h" "*.S" \
        | while IFS= read -r f; do
            # Exclude third_party/ and build/ from incremental set too
            case "$f" in
                third_party/*|build/*|build_script/*) continue ;;
            esac
            printf '%s\n' "$f"
        done)
else
    CANDIDATES=$(collect_all_files)
fi

if [ -z "$CANDIDATES" ]; then
    if [ "$CHECK_MODE" = "incremental" ]; then
        echo "No changed code files vs $BASE — nothing to check, skipped."
    else
        echo "Error: no code files found (after excluding third_party/, build/)."
        exit 1
    fi
    exit 0
fi

N_FILES=$(echo "$CANDIDATES" | wc -l)
if [ "$CHECK_MODE" = "incremental" ]; then
    echo "Checking copyright headers on $N_FILES changed file(s) vs $BASE..."
else
    echo "Checking copyright headers on $N_FILES file(s)..."
fi

# ===================== Check each file =====================
# A valid header must contain both "Copyright (c)" and "SPDX-License-Identifier: MIT"
# within the first MAX_HEADER_LINES lines.

violations=0
violation_list=""

while IFS= read -r f; do
    [ -z "$f" ] && continue

    # Read first N lines, join for pattern search
    header=$(head -n "$MAX_HEADER_LINES" "$f")

    has_copyright=0
    has_spdx=0

    echo "$header" | grep -q "$EXPECTED_COPYRIGHT" && has_copyright=1
    echo "$header" | grep -q "$EXPECTED_SPDX" && has_spdx=1

    if [ "$has_copyright" -eq 0 ] || [ "$has_spdx" -eq 0 ]; then
        violations=$((violations + 1))
        reasons=""
        [ "$has_copyright" -eq 0 ] && reasons="${reasons}missing Copyright line"
        [ "$has_spdx" -eq 0 ] && reasons="${reasons:+$reasons, }missing SPDX-License-Identifier"
        violation_list="${violation_list}  $f ($reasons)\n"
    fi
done <<< "$CANDIDATES"

# ===================== Report =====================

if [ "$violations" -eq 0 ]; then
    echo "copyright check passed."
    exit 0
fi

echo "copyright check failed: $violations file(s) with missing/malformed headers:"
echo
printf "$violation_list"
echo
echo "Expected format (within first $MAX_HEADER_LINES lines):"
echo '  /*'
echo '   * Copyright (c) 2026 hesse'
echo '   *'
echo '   * SPDX-License-Identifier: MIT'
echo '   */'
echo
echo "Fix: ./build_script/add_header.sh (adds headers to files missing them)"
exit 1
