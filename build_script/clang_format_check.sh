#!/bin/bash
# clang_format_check.sh — clang-format format check (+ optional auto-fix)
# Standalone runnable: ./build_script/clang_format_check.sh [base|--all]
# Returns 0 = all files conform (no violations found); 1 = violations detected
# and auto-fixed (files have been modified in-place — git add/commit to apply).
#
# Coverage: all .c/.cc/.h/.hpp in the repo, excluding third_party/ and build/.
# Format standard: repo-root .clang-format (BasedOnStyle: LLVM).
#
# Scope (incremental by default):
#   (no arg)         only files changed vs origin/master (git diff, 3-dot)
#   origin/<branch>  only files changed vs origin/<branch>
#   <branch>         only files changed vs local <branch>
#   --all            full scan (all tracked sources, pre-change behavior)
# Empty incremental result → "nothing changed, skipped", exit 0.
#
# Behavior:
#   default — clang-format --dry-run --Werror to detect violations;
#             if any, auto-fix with clang-format -i.
#             Two clang-format passes total when violations exist.
#
# Invoked by check.sh --filter clang-format.
# Design in doc/design/code_standard.md.

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=_diff_files.sh
source "$SCRIPT_DIR/_diff_files.sh" "${1:-}"

# ===================== Environment check =====================

if ! command -v clang-format &> /dev/null; then
    echo "Error: clang-format is not installed."
    echo "       Install with: sudo apt install clang-format"
    exit 1
fi

echo "clang-format version: $(clang-format --version | head -1)"

# ===================== Collect source files =====================

if [ "$CHECK_MODE" = "full" ]; then
    SOURCES=$(find . -type f \( -name "*.c" -o -name "*.cc" -o -name "*.h" -o -name "*.hpp" \) \
        -not -path "./third_party/*" \
        -not -path "./build/*" \
        | sort)
else
    SOURCES=$(filter_changed "*.c" "*.cc" "*.h" "*.hpp")
    echo "Incremental clang-format: base = $BASE"
fi

if [ -z "$SOURCES" ]; then
    if [ "$CHECK_MODE" = "incremental" ]; then
        echo "No changed .c/.cc/.h/.hpp files vs $BASE — nothing to check, skipped."
    else
        echo "Error: no source files found (after excluding third_party/ and build/)."
        exit 1
    fi
    exit 0
fi

N_SOURCES=$(echo "$SOURCES" | wc -l)
echo "Checking $N_SOURCES file(s)..."

# ===================== Check (pass 1: dry-run) =====================

# --dry-run --Werror: leave files untouched, return non-zero on any violation.
# xargs returns non-zero if any clang-format invocation does.
VIOLATIONS=$(echo "$SOURCES" | xargs --no-run-if-empty clang-format --dry-run --Werror 2>&1)
CHECK_RC=$?

if [ "$CHECK_RC" -eq 0 ]; then
    echo "clang-format check passed."
    exit 0
fi

echo "clang-format check FAILED. Violations:"
echo "$VIOLATIONS"

# ===================== Auto-fix (pass 2: in-place) =====================

echo ""
echo "Applying clang-format -i to fix violations..."
echo "$SOURCES" | xargs --no-run-if-empty clang-format -i
echo ""
echo "Done. git add/commit if satisfied."

exit 1
