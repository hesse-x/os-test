#!/bin/bash
# Cross-build a small target-side LLVM/Clang distribution into the OS sysroot.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
BUILD="$ROOT/build/llvm-target"
SYSROOT="$ROOT/build/sysroot"
SOURCE="$ROOT/third_party/llvm-project/llvm"
NATIVE_TOOLS="${LLVM_NATIVE_TOOL_DIR:-/usr/lib/llvm-18/bin}"
NATIVE_RESOURCE="${LLVM_NATIVE_RESOURCE_DIR:-$(clang -print-resource-dir)}"
JOBS="${LLVM_JOBS:-$(nproc)}"
if [ "$JOBS" -gt 16 ]; then
  JOBS=16
fi

for tool in llvm-tblgen clang-tblgen llvm-config; do
  if [ ! -x "$NATIVE_TOOLS/$tool" ]; then
    echo "FAIL: $NATIVE_TOOLS/$tool is missing (set LLVM_NATIVE_TOOL_DIR)." >&2
    exit 1
  fi
done
for artifact in libc.so libc++.so libc++abi.so libunwind.so libclang_rt.so; do
  if [ ! -e "$SYSROOT/usr/lib/$artifact" ]; then
    echo "FAIL: $SYSROOT/usr/lib/$artifact is missing; run ./build.sh first." >&2
    exit 1
  fi
done

cmake -S "$SOURCE" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/build_script/cmake/toolchain-llvm-x86_64.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG" \
  "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG" \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DLLVM_NATIVE_TOOL_DIR="$NATIVE_TOOLS" \
  -DLLVM_APPEND_VC_REV=OFF \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-linux-musl \
  -DLLVM_BUILD_LLVM_DYLIB=ON \
  -DLLVM_LINK_LLVM_DYLIB=ON \
  -DLLVM_DYLIB_COMPONENTS=all \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_BUILD_UTILS=OFF \
  -DLLVM_BUILD_EXAMPLES=OFF \
  -DLLVM_BUILD_TESTS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_ENABLE_BACKTRACES=OFF \
  -DLLVM_ENABLE_CRASH_OVERRIDES=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_FFI=OFF \
  -DLLVM_ENABLE_BINDINGS=OFF \
  -DCLANG_BUILD_TOOLS=ON \
  -DCLANG_DEFAULT_CXX_STDLIB=libc++ \
  -DCLANG_DEFAULT_LINKER=lld \
  -DCLANG_DEFAULT_RTLIB=compiler-rt \
  -DCLANG_DEFAULT_UNWINDLIB=libunwind \
  -DCLANG_ENABLE_ARCMT=OFF \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DCLANG_INCLUDE_TESTS=OFF

cmake --build "$BUILD" --target LLVM clang lld -j "$JOBS"
DESTDIR="$SYSROOT" cmake --install "$BUILD" --strip --component clang
DESTDIR="$SYSROOT" cmake --install "$BUILD" --strip --component clang-cpp
DESTDIR="$SYSROOT" cmake --install "$BUILD" --component clang-resource-headers
DESTDIR="$SYSROOT" cmake --install "$BUILD" --strip --component LLVM
DESTDIR="$SYSROOT" cmake --install "$BUILD" --strip --component lld

# Stage-0 compiler-rt supplies the generic ELF crt bookends and full builtins.
RUNTIME_DEST="$SYSROOT/usr/lib/clang/18/lib/linux"
install -d "$RUNTIME_DEST"
for runtime in clang_rt.crtbegin-x86_64.o clang_rt.crtend-x86_64.o \
    libclang_rt.builtins-x86_64.a; do
  install -m 644 "$NATIVE_RESOURCE/lib/linux/$runtime" "$RUNTIME_DEST/$runtime"
done

echo "Installed target clang and libLLVM into $SYSROOT/usr."
