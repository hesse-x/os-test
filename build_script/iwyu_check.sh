#!/bin/bash
# iwyu_check.sh — strict include-what-you-use gate
# Standalone runnable: ./build_script/iwyu_check.sh [base|--all]
# Returns 0 = no violations, 1 = add/remove violations or missing environment
#
# Coverage: all .c/.cc in the repo's compile_commands.json,
#           excluding third_party/, skipping .S (iwyu can't analyze assembly).
# Strictness: strict — any should add / should remove is a violation.
#
# Scope (incremental by default):
#   (no arg)         only compile-db .c/.cc changed vs origin/master
#   origin/<branch>  only those changed vs origin/<branch>
#   <branch>         only those changed vs local <branch>
#   --all            all compile-db .c/.cc (pre-change behavior)
# Header-only changes (.h) can't be analyzed by iwyu (it works on TUs); the diff
# is intersected with the compile-db .c/.cc set, and any .h in the diff is noted
# but not analyzed. Empty incremental result → "nothing changed, skipped", exit 0.
#
# Invoked by check.sh --filter iwyu.

# resolve repo root (this script lives in build_script/)
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=_diff_files.sh
source "$SCRIPT_DIR/_diff_files.sh" "${1:-}"

# ===================== Environment check =====================

if ! command -v include-what-you-use &> /dev/null; then
    echo "Error: include-what-you-use is not installed."
    echo "       Install with: sudo apt install include-what-you-use"
    exit 1
fi

CC_JSON=build/compile_commands.json
if [ ! -f "$CC_JSON" ]; then
    echo "Error: $CC_JSON not found. Run ./build.sh first to generate it."
    exit 1
fi

# if ! command -v iwyu_tool &> /dev/null; then
#     echo "Error: iwyu_tool is not installed (should ship with include-what-you-use)."
#     exit 1
# fi

# ===================== Extract file list for analysis =====================
# Take all .c/.cc from compile_commands.json, excluding third_party/ and build/.
# The "file" field in compile_commands is an absolute path (in this repo).
# In incremental mode, intersect with the git-diff file set (rel-to-root paths).
DIFF_RELS=""
if [ "$CHECK_MODE" = "incremental" ]; then
    DIFF_RELS=$(filter_changed "*.c" "*.cc")
    # Note any header changes iwyu can't analyze on its own.
    DIFF_HEADERS=$(filter_changed "*.h" "*.hpp")
    if [ -n "$DIFF_HEADERS" ]; then
        echo "Note: iwyu analyzes TUs only; header changes in this diff are not checked separately:"
        echo "$DIFF_HEADERS" | sed 's/^/    /'
    fi
fi

SOURCES=$(CHECK_MODE="$CHECK_MODE" DIFF_RELS="$DIFF_RELS" python3 - <<'PYEOF'
import json, os, sys
mode = os.environ.get('CHECK_MODE', 'full')
diff_rels = set(filter(None, os.environ.get('DIFF_RELS', '').splitlines()))
with open('build/compile_commands.json') as f:
    data = json.load(f)
out = []
for e in data:
    f = e['file']
    af = f if os.path.isabs(f) else os.path.join(e['directory'], f)
    if 'third_party' in af.split(os.sep):
        continue
    if 'build' in af.split(os.sep):
        continue
    if not (af.endswith('.c') or af.endswith('.cc')):
        continue
    if mode == 'incremental':
        rel = os.path.relpath(af)
        if rel not in diff_rels:
            continue
    out.append(af)
print('\n'.join(out))
PYEOF
)

if [ -z "$SOURCES" ]; then
    if [ "$CHECK_MODE" = "incremental" ]; then
        echo "No changed .c/.cc in compile_commands.json vs $BASE — nothing to check, skipped."
    else
        echo "Error: no .c/.cc sources found in compile_commands.json (after excluding third_party/)."
        exit 1
    fi
    exit 0
fi

N_SOURCES=$(echo "$SOURCES" | wc -l)
if [ "$CHECK_MODE" = "incremental" ]; then
    echo "Running iwyu on $N_SOURCES changed source file(s) vs $BASE (excluding third_party/, skipping .S)..."
else
    echo "Running iwyu on $N_SOURCES source file(s) (excluding third_party/, skipping .S)..."
fi

# ===================== Run iwyu =====================
# iwyu_tool reads each file's full flags from compile_commands, no need to pass -nostdinc/-mno-sse etc. manually.
# --no_comments: drops "// for xxx" comments, cleaner output, easier to parse.
# --max_line_length=200: prevents long lines from being mistakenly folded by iwyu, which would affect parsing.
# --mapping_file: redirect iwyu's built-in glibc symbol attribution (sigset_t-><signal.h>,
#   pid_t-><sys/types.h>, struct timespec-><time.h>, ...) to the project's <xos/...> UAPI
#   headers. Without this, iwyu tells every -nostdinc kernel TU to add <signal.h>/<sys/types.h>
#   — bogus advice that breaks the build. See build_script/iwyu.imp.
IWYU_OUT=$(mktemp /tmp/iwyu-out.XXXXXX)
trap 'rm -f "$IWYU_OUT"' EXIT INT TERM

iwyu_tool -p build -j "$(nproc)" $SOURCES -- -Xiwyu --no_comments -Xiwyu --max_line_length=200 \
    -Xiwyu --mapping_file="$SCRIPT_DIR/iwyu.imp" \
    > "$IWYU_OUT" 2>&1

# ===================== Parse violations =====================
# iwyu_tool always exits 0, output must be parsed.
# Violation = non-empty section after "<path> should add these lines:" ∪ non-empty section after "... should remove ...".
python3 - "$IWYU_OUT" <<'PYEOF'
import re, sys

path = sys.argv[1]
lines = open(path).read().splitlines()
violations = {}  # file -> {'add':[...], 'remove':[...]}

# ---- Spurious-add filter ----------------------------------------------
# iwyu ships a built-in glibc symbol→header attribution table (gcc.libc.imp).
# On this -nostdinc + self-built-libc tree it emits "should add <H>" for glibc
# public headers that DO NOT EXIST in the TU's include search path — pure noise:
# the symbol is already provided by the project's own headers (<xos/...> or the
# user/include/ POSIX wrappers), and following the advice would be a hard
# compile error. We drop those add-lines. Resolvable suggestions (including
# pedantic-but-valid ones like "+ <sys/types.h>" on user TUs, where the wrapper
# exists on disk) are kept — only unresolvable adds are filtered.
#
# Verified unresolvable per TU category (see probe in the .imp investigation):
#   kernel TU (-I src -I include/uapi, NO user/include): every non-<xos>
#     POSIX/glibc public header — <unistd.h>, <sys/types.h>, <sys/time.h>,
#     <inttypes.h>, <sys/socket.h>, <sys/stat.h>, <sys/mman.h>, ...
#   user TU   (adds -I user/include): <sys/time.h>, <inttypes.h> (no wrapper).
FREE_STANDING = {'stdint.h', 'stddef.h', 'stdarg.h', 'stdbool.h'}

def is_kernel_tu(filepath):
    # Match the split used to build the source list: kernel/arch/boot/utils.
    seg = filepath
    for prefix in ('/kernel/', '/arch/', '/boot/', '/utils/'):
        if prefix in seg:
            return True
    return False

def unresolvable_add(line, filepath):
    # Only filter '#include <...>' (angle-bracket public) adds, not "..." or fwd-decls.
    m = re.match(r'\s*#include\s+<([^>]+)>\s*$', line)
    if not m:
        return False
    inc = m.group(1)
    # freestanding C headers resolve everywhere; never filter them.
    if inc in FREE_STANDING:
        return False
    # <xos/...> resolves everywhere (include/uapi is on both paths).
    if inc.startswith('xos/'):
        return False
    if is_kernel_tu(filepath):
        # kernel TUs see only freestanding + <xos/...>: any other angle-bracket
        # public header is an unresolvable glibc-builtin suggestion.
        return True
    # user TUs also have user/include (the POSIX wrappers). Unresolvable there:
    # headers with no wrapper on disk. Verified set: <sys/time.h>, <inttypes.h>.
    return inc in ('sys/time.h', 'inttypes.h')

i, n = 0, len(lines)
while i < n:
    m = re.match(r'^(.*?) should (add|remove) these lines:\s*$', lines[i])
    if m:
        file, kind = m.group(1), m.group(2)
        i += 1
        content = []
        while i < n and lines[i].strip() != '':
            # Stop immediately on next marker (prevents merging after empty sections are skipped)
            if re.match(r'^.* should (add|remove) these lines:\s*$', lines[i]):
                break
            content.append(lines[i])
            i += 1
        if content:
            entry = violations.setdefault(file, {'add': [], 'remove': []})
            if kind == 'add':
                for line in content:
                    if not unresolvable_add(line, file):
                        entry['add'].append(line)
            else:
                entry['remove'] = content
    else:
        i += 1

# Drop files that ended up with no surviving violations (all adds filtered).
violations = {f: v for f, v in violations.items() if v['add'] or v['remove']}

if not violations:
    print("iwyu check passed.")
    sys.exit(0)

print("iwyu check failed: %d file(s) with #include violations:" % len(violations))
print()
for f in sorted(violations):
    v = violations[f]
    # Path relative to repo root, easier to read
    rel = f
    if f.startswith('/home/'):
        parts = f.split('/')
        # Take relative path from repo root (the segment after the 'os-test' part)
        if 'os-test' in parts:
            idx = parts.index('os-test')
            rel = '/'.join(parts[idx + 1:]) or f
    print("  %s" % rel)
    for line in v['add']:
        print("    + %s" % line)
    for line in v['remove']:
        print("    %s" % line)
print()
sys.exit(1)
PYEOF
