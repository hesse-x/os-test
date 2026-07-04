#!/bin/bash
# Add MIT copyright headers to source files.
# Skips: build/, third_party/, build_script/, and .sh files.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR/.."

YEAR="2026"
HOLDER="hesse"

has_header() {
    head -5 "$1" | grep -qE '(Copyright.*'$YEAR'|SPDX-License-Identifier)'
}

add_header() {
    local file="$1" ext="$2"
    local tmpf
    tmpf="$(mktemp /tmp/header_XXXXXX)"

    case "$ext" in
        cmake|txt)
            cat > "$tmpf" <<EOF
# Copyright (c) $YEAR $HOLDER
#
# SPDX-License-Identifier: MIT

EOF
            ;;
        *)
            cat > "$tmpf" <<EOF
/*
 * Copyright (c) $YEAR $HOLDER
 *
 * SPDX-License-Identifier: MIT
 */

EOF
            ;;
    esac

    cat "$tmpf" "$file" > "$file.tmp" && mv "$file.tmp" "$file"
    rm -f "$tmpf"
}

count=0
skipped=0

for ext in c h S cc; do
    while IFS= read -r -d '' f; do
        if has_header "$f"; then
            skipped=$((skipped + 1))
            continue
        fi
        add_header "$f" "$ext"
        echo "+ $f"
        count=$((count + 1))
    done < <(find . -name "*.$ext" \
        -not -path './build/*' \
        -not -path './third_party/*' \
        -not -path './build_script/*' \
        -print0 2>/dev/null)
done

while IFS= read -r -d '' f; do
    if has_header "$f"; then
        skipped=$((skipped + 1))
        continue
    fi
    add_header "$f" "txt"
    echo "+ $f"
    count=$((count + 1))
done < <(find . -name 'CMakeLists.txt' \
    -not -path './build/*' \
    -not -path './third_party/*' \
    -not -path './build_script/*' \
    -print0 2>/dev/null)

echo "---"
echo "Added: $count  Skipped (already has header): $skipped"
