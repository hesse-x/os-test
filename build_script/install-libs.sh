#!/bin/bash
# install-libs.sh — publish library artifacts to a sysroot (companion to install-headers.sh).
#
# Copies build/ library products to build/sysroot/usr/lib/, making the sysroot
# a self-contained cross-target (headers + libs) that Mesa / third-party libs
# can compile/link against with -isysroot, instead of hand-managing -L build.
#
# What gets published:
#   build/libc.a      → $DEST/libc.a      (libc static)
#   build/libc.so     → $DEST/libc.so     (libc shared)
#   build/libm.a      → $DEST/libm.a      (libm static, if present)
#   build/libm.so     → $DEST/libm.so     (libm shared, if present)
#   build/libdrm.a    → $DEST/libdrm.a    (libdrm static)
#   build/libdrm.so   → $DEST/libdrm.so   (libdrm shared, if present)
#   build/libc.so     → $DEST/ld-musl-x86_64.so.1  (fused libc.so is also the
#                       dynamic interpreter; PT_INTERP = /lib/ld-musl-x86_64.so.1)
#
# Dependency order: run AFTER make produces the libraries. libdrm depends on libc,
# so libc is already in place by build time.
#
# Usage:
#   ./install-libs.sh [dest]
#   ./install-libs.sh                        # default: build/sysroot/usr/lib
#   ./install-libs.sh /path/to/sysroot/usr/lib
#
# Verification:
#   ls $DEST → libc.a, libc.so, libdrm.a, ld-musl-x86_64.so.1 (+ libdrm.so, libm.a, libm.so if built)
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$SRC/build}"
DEST="${1:-$BUILD/sysroot/usr/lib}"

echo "Installing libraries → $DEST"
mkdir -p "$DEST"

# Files that are mandatory at this stage (build won't reach a useful state without them).
# The fused libc.so is published under both its soname and the interpreter path
# (mirroring the disk image, ldso.md §3).
mandatory=(
  "libc.a:libc.a"
  "libc.so:libc.so"
  "libc.so:ld-musl-x86_64.so.1"
  "libdrm.a:libdrm.a"
)

# Files that are optional (built by later phases; absent is fine, not a failure).
optional=(
  "libm.a:libm.a"
  "libm.so:libm.so"
  "libdrm.so:libdrm.so"
)

installed=0
for entry in "${mandatory[@]}"; do
  src="${entry%%:*}"
  dst="${entry##*:}"
  if [ -f "$BUILD/$src" ]; then
    cp "$BUILD/$src" "$DEST/$dst"
    echo "  $src → $dst"
    installed=$((installed+1))
  else
    echo "FAIL: required build product $BUILD/$src not found — run ./build.sh first." >&2
    exit 1
  fi
done

for entry in "${optional[@]}"; do
  src="${entry%%:*}"
  dst="${entry##*:}"
  if [ -f "$BUILD/$src" ]; then
    cp "$BUILD/$src" "$DEST/$dst"
    echo "  $src → $dst"
    installed=$((installed+1))
  else
    echo "  (skipping $src: not yet built)"
  fi
done

echo "Installed libraries ($installed file(s)):"
( cd "$DEST" && ls -la )
echo "Done. Sysroot libs ready at $DEST"
