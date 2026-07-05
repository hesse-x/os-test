#!/bin/bash
# clang_format_check.sh — clang-format 格式检查 + 自动修正
# 独立可跑：./build_script/clang_format_check.sh
# 返回 0 = 全部已格式化；1 = 有文件被修正或环境缺失
#
# 覆盖范围：全仓库 .c/.cc/.h/.hpp，排除 third_party/ 和 build/。
# 格式标准：仓库根 .clang-format（BasedOnStyle: LLVM）。
#
# 行为：clang-format -i 自动修正所有格式违规，然后 git diff 展示改动。
#       修正后的差异留在工作树中，用户 git add/commit 即可。
#
# 由 check.sh --filter clang-format 调用。
# 设计见 doc/design/format.md。

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT_DIR"

# ===================== 环境检查 =====================

if ! command -v clang-format &> /dev/null; then
    echo "Error: clang-format is not installed."
    echo "       Install with: sudo apt install clang-format"
    exit 1
fi

echo "clang-format version: $(clang-format --version | head -1)"

# ===================== 收集源文件 =====================

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

# ===================== 修正并展示 diff =====================

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
