#!/bin/bash
# iwyu_check.sh — strict include-what-you-use gate
# Standalone runnable: ./build_script/iwyu_check.sh
# Returns 0 = no violations, 1 = add/remove violations or missing environment
#
# Coverage: all .c/.cc in the repo's compile_commands.json,
#           excluding third_party/, skipping .S (iwyu can't analyze assembly).
# Strictness: strict — any should add / should remove is a violation.
#
# Invoked by check.sh --filter iwyu.

# resolve repo root (this script lives in build_script/)
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT_DIR"

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
SOURCES=$(python3 - <<'PYEOF'
import json, os, sys
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
    if 'build' in af.split(os.sep):
        continue
    if not (af.endswith('.c') or af.endswith('.cc')):
        continue
    out.append(af)
print('\n'.join(out))
PYEOF
)

if [ -z "$SOURCES" ]; then
    echo "Error: no .c/.cc sources found in compile_commands.json (after excluding third_party/)."
    exit 1
fi

N_SOURCES=$(echo "$SOURCES" | wc -l)
echo "Running iwyu on $N_SOURCES source file(s) (excluding third_party/, skipping .S)..."

# ===================== Run iwyu =====================
# iwyu_tool reads each file's full flags from compile_commands, no need to pass -nostdinc/-mno-sse etc. manually.
# --no_comments: drops "// for xxx" comments, cleaner output, easier to parse.
# --max_line_length=200: prevents long lines from being mistakenly folded by iwyu, which would affect parsing.
IWYU_OUT=$(mktemp /tmp/iwyu-out.XXXXXX)
trap 'rm -f "$IWYU_OUT"' EXIT INT TERM

iwyu_tool -p build -j "$(nproc)" $SOURCES -- -Xiwyu --no_comments -Xiwyu --max_line_length=200 \
    > "$IWYU_OUT" 2>&1

# ===================== Parse violations =====================
# iwyu_tool always exits 0, output must be parsed.
# Violation = non-empty section after "<path> should add these lines:" ∪ non-empty section after "... should remove ...".
python3 - "$IWYU_OUT" <<'PYEOF'
import re, sys

path = sys.argv[1]
lines = open(path).read().splitlines()
violations = {}  # file -> {'add':[...], 'remove':[...]}

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
            violations.setdefault(file, {'add': [], 'remove': []})[kind] = content
    else:
        i += 1

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
