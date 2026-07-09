#!/bin/bash
# clang_tidy_check.sh — clang-tidy identifier-naming check
# Standalone runnable: ./build_script/clang_tidy_check.sh [base|--all]
# Returns 0 = all identifiers conform; 1 = violations found; 2 = bad args / env.
#
# Naming standard: repo-root .clang-tidy (readability-identifier-naming, pure
# case-form layer per doc/design/code_standard.md). POSIX/Linux attribution
# and _t-suffix conflicts are handled by the closed allowlist in .clang-tidy
# IgnoredRegexp — grown incrementally from real violations. Everything outside
# the allowlist is a real naming defect.
#
# Scope (incremental by default):
#   (no arg)         only TU changed vs origin/master (3-dot diff)
#   origin/<branch>  only those changed vs origin/<branch>
#   <branch>         only those changed vs local <branch>
#   --all            full scan (all TU in compile database)
# Empty incremental result → "nothing changed, skipped", exit 0.
#
# Exclusions:
#   third_party/ — upstream Unity/gnu-efi/drm, stripped from compile_commands.json
#                   before running AND filtered from findings: first-party TUs
#                   still #include third_party headers (e.g. drm/drm.h), and
#                   HeaderFilterRegex '.*' makes clang-tidy analyze those headers
#                   too, so third_party/ paths are dropped from the final result.
#   .S files     — not in compile DB, clang-tidy cannot parse assembly.
#   Non-naming   — clang-analyzer / other checks may leak in; only
#                   readability-identifier-naming findings are reported.
#
# Behavior: check only, no auto-fix (unlike clang-format which auto-fixes in
#           clang_format_check.sh). Naming violations require human judgement
#           for POSIX/Linux attribution, so auto-fix is inappropriate.
#
# Invoked by check.sh --filter clang-tidy.
# Design in doc/design/code_standard.md.

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=_diff_files.sh
source "$SCRIPT_DIR/_diff_files.sh" "${1:-}"

# ===================== Environment check =====================

if ! command -v clang-tidy &> /dev/null; then
    echo "Error: clang-tidy is not installed."
    echo "       Install with: sudo apt install clang-tidy"
    exit 1
fi

COMPILE_DB="$ROOT_DIR/build/compile_commands.json"
if [ ! -f "$COMPILE_DB" ]; then
    echo "Error: build/compile_commands.json not found (CMAKE_EXPORT_COMPILE_COMMANDS)."
    echo "       Run ./build.sh first to generate the compile database."
    exit 1
fi

if ! command -v jq &> /dev/null; then
    echo "Error: jq is not installed (needed to filter third_party/ from compile DB)."
    echo "       Install with: sudo apt install jq"
    exit 1
fi

echo "clang-tidy version: $(clang-tidy --version | grep -m1 version)"

# ===================== Filtered compile database =====================
# Strip third_party/ entries so clang-tidy never touches upstream TUs.
CLEAN_DB_DIR="$ROOT_DIR/build/clang-tidy-check"
mkdir -p "$CLEAN_DB_DIR"

jq '[.[] | select(.file | test("third_party/") | not)]' "$COMPILE_DB" \
    > "$CLEAN_DB_DIR/compile_commands.json"
N_TU=$(jq 'length' "$CLEAN_DB_DIR/compile_commands.json")
echo "Filtered compile DB: $N_TU TU(s) (third_party/ excluded)"

# ===================== Collect TU list =====================
# TU paths from the compile DB (repo-root-relative for matching with diff output).
ALL_TUS=$(jq -r '.[].file' "$CLEAN_DB_DIR/compile_commands.json" \
    | sed "s|^$ROOT_DIR/||" | sort)

if [ "$CHECK_MODE" = "full" ]; then
    TU_LIST="$ALL_TUS"
else
    # Incremental: intersect changed .c/.cc files with TU list.
    # Only .c/.cc are TU entries; changed headers trigger naming findings
    # naturally via their includers — no need to enumerate them separately.
    CHANGED=$(filter_changed "*.c" "*.cc")
    TU_LIST=""
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        if echo "$ALL_TUS" | grep -qF "$f"; then
            TU_LIST="${TU_LIST}${f}\n"
        fi
    done <<< "$CHANGED"
    TU_LIST=$(echo -e "$TU_LIST" | sort -u)

    echo "Incremental clang-tidy: base = $BASE"
fi

if [ -z "$TU_LIST" ]; then
    if [ "$CHECK_MODE" = "incremental" ]; then
        echo "No changed TU files vs $BASE — nothing to check, skipped."
    else
        echo "Error: no TU files in filtered compile database."
        exit 1
    fi
    exit 0
fi

N_CHECK=$(echo "$TU_LIST" | wc -l)
echo "Checking $N_CHECK TU(s)..."

# ===================== Run =====================
# run-clang-tidy for parallel execution. -p points to filtered DB dir.
# Only report readability-identifier-naming findings — filter out clang-analyzer
# and any other check that leaks in (clang-tidy may activate extra checks from
# its default set despite .clang-tidy specifying only identifier-naming).
# No auto-fix: naming violations need human POSIX/Linux attribution judgement.

WARNFILE=$(mktemp /tmp/clang-tidy-output.XXXXXX)
trap 'rm -f "$WARNFILE"' EXIT INT TERM

echo "$TU_LIST" | run-clang-tidy \
    -p "$CLEAN_DB_DIR" \
    -j "$(nproc)" \
    2>&1 | grep -v '^$\|^Running' > "$WARNFILE"

# Filter to only real naming violations: lines containing both "warning:" (or
# "error:") and "[readability-identifier-naming]". The run-clang-tidy output
# also includes a check-list diagnostic line that merely says
# "readability-identifier-naming" without a warning/error prefix — that is NOT
# a violation, just a "these checks are active" banner.
# Drop findings in third_party/ headers: first-party TUs include upstream
# headers (drm/drm.h, etc.) and HeaderFilterRegex '.*' surfaces their naming,
# which is upstream style we don't own — the per-category IgnoredRegexp in
# .clang-tidy only exempts specific symbols, not whole upstream trees.
NAMING=$(grep -E 'warning:|error:' "$WARNFILE" \
    | grep '\[readability-identifier-naming\]' \
    | grep -v 'third_party/' || true)

if [ -n "$NAMING" ]; then
    N_VIOLATIONS=$(echo "$NAMING" | wc -l)
    echo "clang-tidy naming check FAILED. $N_VIOLATIONS violation(s):"
    echo ""
    echo "$NAMING"
    echo ""
    echo "If a violation is a genuinely exempt symbol (POSIX / spec / framework),"
    echo "add it to .clang-tidy per-category IgnoredRegexp. Otherwise fix the code."
    exit 1
fi

echo "clang-tidy naming check passed."
exit 0
