#!/bin/bash
# clang_format_check.sh — clang-format format check (+ optional auto-fix)
# Standalone runnable: ./build_script/clang_format_check.sh [--fix]
# Returns 0 = check passed (all files conform); 1 = check failed (violations
# remain, or the environment is missing). The return value always reflects the
# check result; --fix only applies repairs after a failed check, it does not
# change the exit status.
#
# Coverage: all .c/.cc/.h/.hpp in the repo, excluding third_party/ and build/.
# Format standard: repo-root .clang-format (BasedOnStyle: LLVM).
#
# Behavior:
#   default  — check only (clang-format --dry-run --Werror). No file is modified.
#   --fix    — check first; if the check fails, run clang-format -i to auto-fix,
#              then show the changes via git diff. Two clang-format passes total.
#
# Invoked by check.sh --filter clang-format.
# Design in doc/design/code_standard.md.

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT_DIR"

# ===================== Parse arguments =====================

FIX=0
for arg in "$@"; do
    case "$arg" in
        --fix) FIX=1 ;;
        *) echo "Usage: $0 [--fix]"; exit 1 ;;
    esac
done

# ===================== Environment check =====================

if ! command -v clang-format &> /dev/null; then
    echo "Error: clang-format is not installed."
    echo "       Install with: sudo apt install clang-format"
    exit 1
fi

echo "clang-format version: $(clang-format --version | head -1)"

# ===================== Collect source files =====================

SOURCES=$(find . -type f \( -name "*.c" -o -name "*.cc" -o -name "*.h" -o -name "*.hpp" \) \
    -not -path "./third_party/*" \
    -not -path "./build/*" \
    | sort)

if [ -z "$SOURCES" ]; then
    echo "Error: no source files found (after excluding third_party/ and build/)."
    exit 1
fi

N_SOURCES=$(echo "$SOURCES" | wc -l)
echo "Checking $N_SOURCES file(s)..."

# ===================== Check (pass 1: dry-run) =====================

# --dry-run --Werror: leave files untouched, return non-zero on any violation.
# xargs returns non-zero if any clang-format invocation does.
VIOLATIONS=$(echo "$SOURCES" | xargs clang-format --dry-run --Werror 2>&1)
CHECK_RC=$?

if [ "$CHECK_RC" -eq 0 ]; then
    echo "clang-format check passed."
    exit 0
fi

echo "clang-format check FAILED. Violations:"
echo "$VIOLATIONS"

# ===================== Optional auto-fix (pass 2: in-place) =====================

if [ "$FIX" -eq 1 ]; then
    echo ""
    echo "Applying clang-format -i to fix violations..."
    echo "$SOURCES" | xargs clang-format -i
    echo ""
    echo "Fixed changes (git diff):"
    git diff
    echo ""
    echo "Review and git add/commit."
fi

exit 1
