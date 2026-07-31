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
#   build/musl/lib/{crt1,Scrt1,crti,crtn}.o → $DEST/*.o  (musl crt objects, so
#                       --sysroot cross consumers resolve crt automatically)
#   generated stub .so → $DEST/{librt,libdl,libpthread,libresolv,libxnet}.so
#                       (one-line `INPUT(libc.so)` linker scripts: musl folds
#                       these into libc, but -lrt/-ldl/-lpthread lookups need
#                       the names to exist)
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
#   ls $DEST → libc.a, libc.so, libdrm.a, ld-musl-x86_64.so.1
#              + crt1.o, Scrt1.o, crti.o, crtn.o
#              + librt/libdl/libpthread/libresolv/libxnet.so (INPUT(libc.so) stubs)
#              + libdrm.so, libm.a, libm.so if built
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

# musl crt objects — produced by the musl subproject (musl_rules.cmake) under
# build/musl/lib/, NOT under build/. Cross-toolchain consumers (libc++ runtimes
# build, Mesa cross-file) drive the compiler with --sysroot and let it resolve
# crt itself, so the crt .o must live inside the sysroot (not just be referenced
# by absolute path as the OS's own ELF links do via user_rules.cmake). musl's
# libc.a/libc.so are intentionally NOT installed from here — they come from the
# mandatory block above (build/libc.*).
musl_crt_dir="$BUILD/musl/lib"
crt_objects=(
  "crt1.o"   # static _start (non-PIC)
  "Scrt1.o"  # dynamic _start (PIC)
  "crti.o"   # .init bracket
  "crtn.o"   # .fini bracket
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

# Publish musl crt objects into the sysroot (see comment at crt_objects above).
for crt in "${crt_objects[@]}"; do
  if [ -f "$musl_crt_dir/$crt" ]; then
    cp "$musl_crt_dir/$crt" "$DEST/$crt"
    echo "  musl/lib/$crt → $crt"
    installed=$((installed+1))
  else
    echo "FAIL: musl crt $musl_crt_dir/$crt not found — build the musl subproject first." >&2
    exit 1
  fi
done

# Compat stub .so for libraries musl folds into libc: rt, dl, pthread, resolv,
# xnet. glibc ships these as separate .so; musl provides all their symbols from
# libc.so. Cross consumers (Mesa links -lrt -ldl -lpthread; libc++ runtimes link
# -lpthread/-lrt) need the names to resolve, so each stub is a one-line linker
# script `INPUT(libc.so)` redirecting the -l lookup to the fused libc. This
# matches the Mesa cross-file's stated precondition (crt + stubs in sysroot).
for stub in librt libdl libpthread libresolv libxnet; do
  printf 'INPUT(libc.so)\n' > "$DEST/$stub.so"
  echo "  stub $stub.so → INPUT(libc.so)"
  installed=$((installed+1))
done

echo "Installed libraries ($installed file(s)):"
( cd "$DEST" && ls -la )
echo "Done. Sysroot libs ready at $DEST"
