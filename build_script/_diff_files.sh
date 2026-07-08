#!/bin/bash
# _diff_files.sh — shared incremental file-list resolver for the check scripts
#
# Sourced (not executed) by clang_format_check.sh / iwyu_check.sh / sparse_check.sh.
# Parses the caller's first positional argument to decide incremental-vs-full and
# the git diff base, then exports:
#   CHECK_MODE     — "incremental" | "full"
#   BASE           — the resolved diff base (only set in incremental mode)
#   CHANGED_FILES  — newline-separated, repo-root-relative paths of files changed
#                    vs BASE (deleted files already excluded via --diff-filter=d)
#
# Provides:
#   filter_changed <glob> [<glob> ...]
#       Print the subset of CHANGED_FILES whose basename matches any of the given
#       globs (e.g. '*.c' '*.cc'). Empty result is normal and not an error.
#
# Argument convention (shared across all check scripts):
#   (none)            incremental, base = origin/master
#   --all             full scan (pre-change behavior)
#   origin/<branch>   incremental, base = origin/<branch>   (remote)
#   <branch>          incremental, base = <branch>          (local branch)
#   <any-ref>         incremental, base = <ref>             (tag/hash/etc.)
#
# Invalid base → stderr message + exit 2.

# Resolve repo root from the caller's SCRIPT_DIR (each check script sets it).
if [ -z "$SCRIPT_DIR" ]; then
    echo "Internal error: SCRIPT_DIR must be set before sourcing _diff_files.sh" >&2
    exit 2
fi
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT_DIR"

_diff_files_parse() {
    local arg="$1"

    if [ "$arg" = "--all" ]; then
        CHECK_MODE=full
        BASE=""
        CHANGED_FILES=""
        return 0
    fi

    # Default and explicit base: incremental.
    CHECK_MODE=incremental
    BASE="${arg:-origin/master}"

    # Validate the base ref. `git rev-parse --verify <ref>` accepts anything
    # peelable to a commit (branches, tags, hashes, HEAD, remote refs). We pass
    # the raw ref so that "origin/master" / "plan" / a short hash all resolve.
    if ! git rev-parse --verify --quiet "$BASE^{commit}" >/dev/null 2>&1; then
        echo "Error: cannot resolve diff base '$BASE'." >&2
        echo "       Use --all for a full scan, or pass a branch/ref name." >&2
        echo "       Local branches : $(git for-each-ref --format='%(refname:short)' refs/heads | tr '\n' ' ')" >&2
        echo "       Remote branches: $(git for-each-ref --format='%(refname:short)' refs/remotes | tr '\n' ' ')" >&2
        exit 2
    fi

    # Three-dot diff against the merge-base: only files this branch added/modified
    # relative to BASE. Same convention as doc/design/code_standard.md "incremental runs".
    # --diff-filter=d drops deleted files (can't lint a file that no longer exists).
    CHANGED_FILES=$(git diff --name-only --diff-filter=d "$BASE"...HEAD)
}

# filter_changed <glob> [<glob> ...]
# Print CHANGED_FILES entries matching any glob, one per line.
filter_changed() {
    [ -z "$CHANGED_FILES" ] && return 0
    # extglob not needed; use case-glob over basenames.
    local f base
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        base="${f##*/}"
        for pat in "$@"; do
            # shellcheck disable=SC2053  (glob match, not string equality)
            [[ "$base" == $pat ]] && { printf '%s\n' "$f"; break; }
        done
    done <<< "$CHANGED_FILES"
}

_diff_files_parse "${1:-}"
