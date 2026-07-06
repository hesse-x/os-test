#!/bin/bash
# clang_format_check.sh — clang-format format check + auto-fix
# Standalone runnable: ./build_script/clang_format_check.sh
# Returns 0 = all formatted; 1 = files were fixed or missing environment
#
# Coverage: all .c/.cc/.h/.hpp in the repo, excluding third_party/ and build/.
# Format standard: repo-root .clang-format (BasedOnStyle: LLVM).
#
# Behavior: clang-format -i auto-fixes all format violations, then git diff shows the changes.
#       The fixed diff stays in the working tree; user can git add/commit.
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

# ===================== Fix and show diff =====================

echo "$SOURCES" | xargs clang-format -i

DIFF=$(git diff)

if [ -z "$DIFF" ]; then
    echo "clang-format check passed."
    exit 0
fi

echo ""
echo "$DIFF"
echo ""
echo "clang-format check: $(git diff --stat | tail -1). Review and git add/commit."
exit 1
