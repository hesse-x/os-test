#!/bin/bash
# Preserve every image ELF by build ID for offline perf symbolization.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
MANIFEST="$BUILD_DIR/image_manifest.txt"
SYMBOL_DIR="$BUILD_DIR/perf-symbols"
BUILD_ID_MANIFEST="$SYMBOL_DIR/build-id-manifest.tsv"
TEST_MANIFEST="$SYMBOL_DIR/test-manifest.tsv"

mkdir -p "$SYMBOL_DIR"
: > "$BUILD_ID_MANIFEST"

while IFS=$'\t' read -r artifact image_path partition; do
    [ -n "$artifact" ] || continue
    case "$artifact" in '#'* ) continue ;; esac

    source_file="$BUILD_DIR/$artifact"
    magic="$(LC_ALL=C od -An -tx1 -N4 "$source_file" | tr -d ' \n')"
    if [ "$magic" != "7f454c46" ]; then
        continue
    fi

    # Link benchmark inputs are shipped in the image but never execute as a
    # process, so they have no runtime addresses to symbolize and need no ID.
    elf_type="$(LC_ALL=C readelf -h "$source_file" | awk '/^[[:space:]]*Type:/ {print $2; exit}')"
    if [ "$elf_type" = "REL" ]; then
        continue
    fi

    build_id="$(LC_ALL=C readelf -n "$source_file" | awk '/Build ID:/ {print $3; exit}')"
    if [ -z "$build_id" ]; then
        echo "perf-symbols: ELF lacks build ID: $artifact" >&2
        exit 1
    fi

    destination="$SYMBOL_DIR/$artifact"
    mkdir -p "$(dirname "$destination")"
    cp "$source_file" "$destination"
    printf '%s\t%s\t%s\t%s\n' "$build_id" "$artifact" "$image_path" "$partition" \
        >> "$BUILD_ID_MANIFEST"
done < "$MANIFEST"

LC_ALL=C sort -u -o "$BUILD_ID_MANIFEST" "$BUILD_ID_MANIFEST"
awk -F'"' '
    /^[[:space:]]*\{"[^"]+",[[:space:]]*"[^"]+"\},?[[:space:]]*$/ {
        printf "%d\t%s\t%s\n", ++id, $2, $4
    }
' "$PROJECT_DIR/user/test/test_runner.c" > "$TEST_MANIFEST"
if [ ! -s "$TEST_MANIFEST" ]; then
    echo "perf-symbols: no test entries found in test_runner.c" >&2
    exit 1
fi
echo "perf-symbols: wrote $BUILD_ID_MANIFEST"
echo "perf-symbols: wrote $TEST_MANIFEST"
