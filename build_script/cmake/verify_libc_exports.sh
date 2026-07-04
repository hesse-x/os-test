#!/bin/bash
# 构建期断言：libc.so 的导出符号集合必须 == libc.map 的 global: 列表
# 用法：verify_libc_exports.sh <libc.so> <libc.map>
#
# 规则（plan_posix D5/D6）：头文件即契约 + 显式 global 白名单 + local: * 兜底。
# 漂移即 FATAL：
#   - libc.so 实际导出（dynsym GLOBAL FUNC/OBJECT）却不在 libc.map global → 未审 ABI 泄漏
#   - libc.map global 列了但 libc.so 未定义 → 声明未实现（或拼错）
# 版本节点符号（如 LIBC_1.0）、UNDEF（外部引用）、NOTYPE（如 _end）不计入。
so="$1"
map="$2"
if [ -z "$so" ] || [ -z "$map" ]; then
    echo "usage: $0 <libc.so> <libc.map>"
    exit 2
fi
if [ ! -f "$so" ] || [ ! -f "$map" ]; then
    echo "FATAL: missing $so or $map"
    exit 2
fi

# 实际导出：dynsym 中 GLOBAL/WEAK 的 FUNC/OBJECT 符号名（排除 UND 未定义引用）
# readelf -sW --dyn-syms 列：Num: Value Size Type Bind Vis Ndx Name
#   $4=Type(FUNC/OBJECT) $5=Bind(GLOBAL/WEAK) $7=Ndx $8=Name
# 版本脚本生效后符号名带版本后缀（name@@LIBC_1.0），剥掉比较
# Ndx=ABS 的是版本节点本身（LIBC_1.0）不是代码符号，排除
actual=$(readelf -sW --dyn-syms "$so" \
    | awk '($5=="GLOBAL" || $5=="WEAK") && ($4=="FUNC" || $4=="OBJECT") && $7!="UND" && $7!="ABS" {print $8}' \
    | sed -E 's/@@?[^@]*$//' \
    | sort -u)

# 期望导出：libc.map 中 global: 段下的符号（去注释、去版本节点、去 local 段）
# 符号以 ';' 分隔（可同行多个），如 `fclose; fflush; fgetc;`
expected=$(awk '
    /^[[:space:]]*global:[[:space:]]*$/ {ing=1; next}
    /^[[:space:]]*local:[[:space:]]*$/  {ing=0; next}
    !ing {next}
    # 去行内 /* */ 注释
    {line=$0; sub(/\/\*.*\*\//,"",line); print line}
' "$map" | tr ';' '\n' \
    | sed -e 's/[{}*]//g' -e 's/[[:space:]]//g' \
    | grep -vE '^(|global|local)$' \
    | sort -u)

# 对比
extra=$(comm -23 <(echo "$actual") <(echo "$expected"))
missing=$(comm -13 <(echo "$actual") <(echo "$expected"))

rc=0
if [ -n "$extra" ]; then
    echo "FATAL: libc.so exports symbols not in libc.map global: (ABI leak)"
    echo "$extra" | sed 's/^/  + /'
    rc=1
fi
if [ -n "$missing" ]; then
    echo "FATAL: libc.map global: lists symbols not exported by libc.so (unimplemented/typo)"
    echo "$missing" | sed 's/^/  - /'
    rc=1
fi
if [ $rc -eq 0 ]; then
    n=$(echo "$actual" | grep -c .)
    echo "OK: libc.so exports match libc.map ($n symbols)"
fi
exit $rc
