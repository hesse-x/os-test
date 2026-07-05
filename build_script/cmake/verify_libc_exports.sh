#!/bin/bash
# Build-time assertion: libc.so exported symbol set must == libc.map global: list
# Usage: verify_libc_exports.sh <libc.so> <libc.map>
#
# Rules (plan_posix D5/D6): headers are the contract + explicit global whitelist + local: * fallback.
# Drift is FATAL:
#   - libc.so actually exports (dynsym GLOBAL FUNC/OBJECT) but not in libc.map global → unreviewed ABI leak
#   - libc.map global lists it but libc.so doesn't define → declared but unimplemented (or typo)
# Version node symbols (e.g. LIBC_1.0), UNDEF (external references), NOTYPE (e.g. _end) are not counted.
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

# Actual exports: GLOBAL/WEAK FUNC/OBJECT symbol names in dynsym (excluding UND undefined references)
# readelf -sW --dyn-syms columns: Num: Value Size Type Bind Vis Ndx Name
#   $4=Type(FUNC/OBJECT) $5=Bind(GLOBAL/WEAK) $7=Ndx $8=Name
# After version script takes effect, symbol names carry version suffix (name@@LIBC_1.0), strip it for comparison
# Ndx=ABS entries are version nodes themselves (LIBC_1.0), not code symbols, exclude them
actual=$(readelf -sW --dyn-syms "$so" \
    | awk '($5=="GLOBAL" || $5=="WEAK") && ($4=="FUNC" || $4=="OBJECT") && $7!="UND" && $7!="ABS" {print $8}' \
    | sed -E 's/@@?[^@]*$//' \
    | sort -u)

# Expected exports: symbols under the global: section in libc.map (strip comments, version nodes, local section)
# Symbols are ';' separated (multiple per line allowed), e.g. `fclose; fflush; fgetc;`
expected=$(awk '
    /^[[:space:]]*global:[[:space:]]*$/ {ing=1; next}
    /^[[:space:]]*local:[[:space:]]*$/  {ing=0; next}
    !ing {next}
    # Strip inline /* */ comments
    {line=$0; sub(/\/\*.*\*\//,"",line); print line}
' "$map" | tr ';' '\n' \
    | sed -e 's/[{}*]//g' -e 's/[[:space:]]//g' \
    | grep -vE '^(|global|local)$' \
    | sort -u)

# Compare
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
