#!/bin/bash
# iwyu_check.sh — include-what-you-use 严格 gate
# 独立可跑：./build_script/iwyu_check.sh
# 返回 0 = 无违规，1 = 有 add/remove 违规或环境缺失
#
# 覆盖范围：全仓库 compile_commands.json 中的 .c/.cc，
#           排除 third_party/，跳过 .S（iwyu 无法分析汇编）。
# 执行强度：严格 —— 任何 should add / should remove 都判违规。
#
# 由 check.sh --filter iwyu 调用。

# resolve repo root (this script lives in build_script/)
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT_DIR"

# ===================== 环境检查 =====================

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

# ===================== 提取分析文件列表 =====================
# 从 compile_commands.json 取所有 .c/.cc，排除 third_party/ 和 build/。
# compile_commands 的 "file" 字段是绝对路径（本仓库如此）。
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

# ===================== 跑 iwyu =====================
# iwyu_tool 从 compile_commands 读每文件完整 flags，无需手传 -nostdinc/-mno-sse 等。
# --no_comments：去掉 "// for xxx" 注释，输出更干净，便于解析。
# --max_line_length=200：避免长行被 iwyu 误折叠影响解析。
IWYU_OUT=$(mktemp /tmp/iwyu-out.XXXXXX)
trap 'rm -f "$IWYU_OUT"' EXIT INT TERM

iwyu_tool -p build -j "$(nproc)" $SOURCES -- -Xiwyu --no_comments -Xiwyu --max_line_length=200 \
    > "$IWYU_OUT" 2>&1

# ===================== 解析违规 =====================
# iwyu_tool 退出码恒为 0，必须解析输出。
# 违规 = "<path> should add these lines:" 后非空段 ∪ "... should remove ..." 后非空段。
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
            # 遇到下一个 marker 立即停（防止空段被跳过后粘连）
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
    # 仓库根相对路径，更易读
    rel = f
    if f.startswith('/home/'):
        parts = f.split('/')
        # 取从仓库根（含 'os-test' 那段之后）的相对路径
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
