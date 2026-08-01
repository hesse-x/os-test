#!/bin/bash
# build_libcxx.sh — opt-in standalone build of libc++ (LLVM runtimes sub-build)
# installed into build/sysroot.
#
# Default ./build.sh does not invoke this; --cxx (or manually
# `bash build_script/build_libcxx.sh`) builds it. If already installed, returns
# instantly without rebuilding; if prerequisites are missing, errors out with a
# clear message instead of producing obscure cmake link errors.
#
# See refact_cmake.md for the finalized reproducible opt-in standalone build.
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$SRC/build}"
SYSROOT="${SYSROOT:-$BUILD/sysroot}"
LIBCXX_BUILD="$BUILD/libcxx-build"
# Probe anchors: libc++.so in sysroot (symlink→libc++.so.1→libc++.so.1.0) + the
# header tree. Both conditions must hold to count as "fully installed" — checking
# only .so may miss headers, and vice versa.
LIBCXX_SO="$SYSROOT/usr/lib/libc++.so"
LIBCXX_HEADERS="$SYSROOT/usr/include/c++/v1"

# ---- 0. Probe: if already built, skip (no rebuild) ----
if [ -e "$LIBCXX_SO" ] && [ -d "$LIBCXX_HEADERS" ] && \
   readelf -d "$LIBCXX_SO" 2>/dev/null | grep -Fq "Shared library: [libclang_rt.so]"; then
  echo "libc++ already installed at $SYSROOT with libclang_rt.so — nothing to do."
  exit 0
fi

if [ -e "$LIBCXX_SO" ] || [ -d "$LIBCXX_HEADERS" ]; then
  echo "libc++ installation lacks libclang_rt.so dependency — rebuilding."
fi

# ---- 1. Precheck: sysroot must be ready (crt + stub + headers + libc.so exported symbols) ----
# These are exactly install-libs.sh's mandatory block + install-headers.sh's
# products, covering every gap the libc++ build has hit (cpp_worklist "fixed"
# section). Prechecking turns "undefined during compile" into "missing
# prerequisite at entry + guidance".
check_sysroot() {
  local miss=0
  # crt (install-libs.sh copies from build/musl/lib)
  for o in crt1.o Scrt1.o crti.o crtn.o; do
    [ -f "$SYSROOT/usr/lib/$o" ] || { echo "FAIL: $SYSROOT/usr/lib/$o missing — run ./build.sh first." >&2; miss=1; }
  done
  # libc (fused libc.so is both library and interpreter) and compiler-rt int128 runtime.
  [ -f "$SYSROOT/usr/lib/libc.so" ] || { echo "FAIL: libc.so missing — run ./build.sh first." >&2; miss=1; }
  [ -f "$SYSROOT/usr/lib/libclang_rt.so" ] || { echo "FAIL: libclang_rt.so missing — run ./build.sh first." >&2; miss=1; }
  [ -f "$SYSROOT/usr/lib/ld-musl-x86_64.so.1" ] || { echo "FAIL: ld-musl interpreter missing — run ./build.sh first." >&2; miss=1; }
  # stub .so (the 5 INPUT(libc.so) scripts musl folds into libc)
  for s in librt libdl libpthread libresolv libxnet; do
    [ -f "$SYSROOT/usr/lib/$s.so" ] || { echo "FAIL: stub $s.so missing — run ./build.sh first." >&2; miss=1; }
  done
  # Header tree (install-headers.sh output, including libc++-needed
  # link.h/elf.h/nl_types.h/langinfo.h/sys/syscall.h/linux/futex.h)
  [ -d "$SYSROOT/usr/include" ] || { echo "FAIL: $SYSROOT/usr/include missing — run install-headers.sh (via ./build.sh)." >&2; miss=1; }
  if [ "$miss" -ne 0 ]; then
    echo "Sysroot not ready. Run ./build.sh (default flow) before --cxx." >&2
    exit 1
  fi
}
check_sysroot

# ---- 2. Precheck: clang-18 + llvm-project submodule ready ----
# The runtimes sub-build requires clang-18 (the release/18.x submodule's libcxx
# source is aligned with clang-18).
command -v clang++ >/dev/null 2>&1 || { echo "FAIL: clang++ not found (runtimes needs clang-18)." >&2; exit 1; }
[ -d "$SRC/third_party/llvm-project/runtimes" ] || {
  echo "FAIL: llvm-project runtimes/ missing — run:" >&2
  echo "  git submodule update --init third_party/llvm-project" >&2
  exit 1
}

# ---- 3. Configure + build + install back into sysroot ----
# Args match the verified-working set from cpp_worklist.md "build reproduction":
#   -DLLVM_ENABLE_RUNTIMES           build the three together
#   -DCMAKE_SYSROOT + --sysroot       cross sysroot isolation (memory [[mesa-cross-sysroot-isolation]])
#   -nodefaultlibs -lc               drop implicit -lc then re-add (Scrt1.o _start calls __libc_start_main)
#   -nostdinc++                       drop compiler's own C++ headers, use only sysroot's c++/v1
#   -DLIBCXX_HAS_MUSL_LIBC=ON         take the default rune table (musl is not on libc++'s platform list)
#   -DCMAKE_INSTALL_PREFIX=sysroot/usr install back into sysroot (not /usr/local)
echo "Configuring libc++ runtimes (cmake -S runtimes) → $LIBCXX_BUILD"
cmake -G Ninja -S "$SRC/third_party/llvm-project/runtimes" -B "$LIBCXX_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_AR=/usr/bin/ar -DCMAKE_RANLIB=/usr/bin/ranlib \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
  -DCMAKE_INSTALL_PREFIX="$SYSROOT/usr" -DCMAKE_SYSROOT="$SYSROOT" \
  -DCMAKE_C_FLAGS="--sysroot=$SYSROOT -nodefaultlibs -I$SYSROOT/usr/include" \
  -DCMAKE_CXX_FLAGS="--sysroot=$SYSROOT -nodefaultlibs -I$SYSROOT/usr/include -nostdinc++" \
  -DCMAKE_EXE_LINKER_FLAGS="--sysroot=$SYSROOT -nodefaultlibs -lc" \
  -DCMAKE_SHARED_LINKER_FLAGS="--sysroot=$SYSROOT -nodefaultlibs -Wl,--no-as-needed -lclang_rt -Wl,--as-needed" \
  -DLIBCXX_HAS_MUSL_LIBC=ON \
  -DLIBCXX_ENABLE_SHARED=ON -DLIBCXX_ENABLE_STATIC=ON \
  -DLIBCXXABI_ENABLE_SHARED=ON -DLIBCXXABI_ENABLE_STATIC=ON \
  -DLIBUNWIND_ENABLE_SHARED=ON -DLIBUNWIND_ENABLE_STATIC=ON \
  -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF

echo "Building libc++ (ninja) → $LIBCXX_BUILD"
ninja -C "$LIBCXX_BUILD"

echo "Installing libc++ → $SYSROOT/usr/{lib,include/c++/v1}"
ninja -C "$LIBCXX_BUILD" install
# Products: $SYSROOT/usr/lib/{libc++,libc++abi,libunwind}.so{,.1,.1.0} + .a
#           $SYSROOT/usr/include/c++/v1/*  (including __config_site / module.modulemap)

echo "Done. libc++ installed at $SYSROOT (re-run ./build.sh [--cxx] to ship into disk.img)."
