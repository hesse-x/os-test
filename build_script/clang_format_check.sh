#!/bin/bash
# clang_format_check.sh — clang-format format check (+ optional auto-fix)
# Standalone runnable: ./build_script/clang_format_check.sh
# Returns 0 = all files conform (no violations found); 1 = violations detected
# and auto-fixed (files have been modified in-place — git add/commit to apply).
#
# Coverage: all .c/.cc/.h/.hpp in the repo, excluding third_party/ and build/.
# Format standard: repo-root .clang-format (BasedOnStyle: LLVM).
#
# Behavior:
#   default — clang-format --dry-run --Werror to detect violations;
#             if any, auto-fix with clang-format -i.
#             Two clang-format passes total when violations exist.
#
# Invoked by check.sh --filter clang-format.
# Design in doc/design/code_standard.md.

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT_DIR"

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

# ===================== Auto-fix (pass 2: in-place) =====================

echo ""
echo "Applying clang-format -i to fix violations..."
echo "$SOURCES" | xargs clang-format -i
echo ""
echo "Done. git add/commit if satisfied."

exit 1
