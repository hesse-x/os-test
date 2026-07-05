#!/bin/bash
# check.sh — 检查调度器
#
# 用法:
#   ./check.sh                          # 跑全部四项（按序）
#   ./check.sh --filter sparse,iwyu     # 只跑指定项，按给定顺序
#   ./check.sh --filter iwyu            # 单项
#
# 合法检查项: sparse, iwyu, clang-format, clang-tidy
#   clang-tidy 暂为占位（见 doc/design/format.md）。
#
# 退出码: 全部通过 0；任一失败 1；参数错误 2。

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# 检查项 → 子脚本 映射
declare -A CHECK_SCRIPT=(
    [sparse]="build_script/sparse_check.sh"
    [iwyu]="build_script/iwyu_check.sh"
    [clang-format]="build_script/clang_format_check.sh"
    [clang-tidy]="build_script/clang_tidy_check.sh"
)
ALL_CHECKS=(sparse iwyu clang-format clang-tidy)

# ===================== 解析 --filter =====================
FILTER=""
FILTER_SET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --filter)
            FILTER="$2"
            FILTER_SET=1
            shift 2
            ;;
        --filter=*)
            FILTER="${1#--filter=}"
            FILTER_SET=1
            shift
            ;;
        -h|--help)
            sed -n '2,12p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 [--filter sparse,iwyu,...]"
            exit 2
            ;;
    esac
done

# 展开 filter 为有序列表；未传 --filter 则全跑
if [ "$FILTER_SET" -eq 0 ]; then
    CHECKS=("${ALL_CHECKS[@]}")
else
    if [ -z "$FILTER" ]; then
        echo "Error: --filter value is empty. Valid: ${ALL_CHECKS[*]}"
        exit 2
    fi
    CHECKS=()
    IFS=',' read -ra _parts <<< "$FILTER"
    for c in "${_parts[@]}"; do
        if [ -z "$c" ]; then
            echo "Error: empty check name in --filter '$FILTER'. Valid: ${ALL_CHECKS[*]}"
            exit 2
        fi
        if [ -z "${CHECK_SCRIPT[$c]}" ]; then
            echo "Error: unknown check '$c'. Valid: ${ALL_CHECKS[*]}"
            exit 2
        fi
        CHECKS+=("$c")
    done
fi

# ===================== 顺序执行 =====================
FAIL=0
STEP=0
for c in "${CHECKS[@]}"; do
    STEP=$((STEP + 1))
    echo ""
    echo "=== Step $STEP: $c ==="
    bash "$SCRIPT_DIR/${CHECK_SCRIPT[$c]}"
    rc=$?
    if [ $rc -ne 0 ]; then
        FAIL=1
        echo "Step $STEP ($c) FAILED (exit $rc)."
    fi
done

# ===================== 汇总 =====================
echo ""
if [ "$FAIL" -ne 0 ]; then
    echo "check.sh: FAILED (one or more checks did not pass)."
    exit 1
fi
echo "check.sh: all selected checks passed."
exit 0
