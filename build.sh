#!/bin/bash
# build.sh - CMake builds kernel + EFI bootloader + userspace ELF + generates image
set -e

# Configure git hooks path so pre-push check works out of the box
git config core.hooksPath build_script/githooks

# Build type: default Release, -d for Debug (with -g debug info)
BUILD_TYPE=Release
CMAKE_EXTRA=""
# Compiler: default clang, --gcc to switch. Both toolchains are supported.
OS_COMPILER=clang

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d)
            BUILD_TYPE=Debug
            shift
            ;;
        --test)
            CMAKE_EXTRA="$CMAKE_EXTRA -DTEST=1"
            shift
            ;;
        --sanitizer)
            CMAKE_EXTRA="$CMAKE_EXTRA -DSANITIZE=1"
            shift
            ;;
        --perf)
            CMAKE_EXTRA="$CMAKE_EXTRA -DPERF=1"
            shift
            ;;
        --gcc)
            OS_COMPILER=gcc
            shift
            ;;
        --clang)
            OS_COMPILER=clang
            shift
            ;;
        --cxx)
            # opt-in: build libc++ (LLVM runtimes) into build/sysroot after the
            # default flow, so mkdisk can ship it into disk.img. Default flow
            # does not build libc++ (zero cost). See refact_cmake.md.
            BUILD_LIBCXX=1
            shift
            ;;
        *)
            echo "Usage: $0 [-d] [--test] [--sanitizer] [--perf] [--gcc] [--clang] [--cxx]"
            exit 1
            ;;
    esac
done

# Ensure SANITIZE is explicitly set so CMake cache doesn't retain stale values
if ! echo "$CMAKE_EXTRA" | grep -q "SANITIZE="; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DSANITIZE=0"
fi

# --cxx also tells CMake to build the libc++ smoke ELF + wire its test_runner entry
# (-DLIBCXX=1). Kept in sync with the post-build build_libcxx.sh invocation below.
if [ "${BUILD_LIBCXX:-0}" = "1" ]; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DLIBCXX=1"
fi

# 1. CMake build (kernel + userspace)
mkdir -p build && cd build
cmake -GNinja \
      -DCMAKE_TOOLCHAIN_FILE=../build_script/cmake/toolchain-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
      -DOS_COMPILER=$OS_COMPILER \
      $CMAKE_EXTRA \
      ..
ninja
cd ..

# install-headers.sh reads CC to locate the matching freestanding include dir
# (gcc → …/gcc/<v>/include, clang → …/clang/<v>/include).
if [[ "$OS_COMPILER" == "gcc" ]]; then
    export CC=gcc
else
    export CC=clang
fi

# 2. Publish sysroot artifacts (UAPI headers + libs → self-contained cross-target)
bash build_script/install-headers.sh
bash build_script/install-libs.sh

# 2b. (opt-in) Build libc++ into the sysroot. Must run AFTER install-headers/install-libs
# (sysroot ready: crt + stub + headers + libc.so exports) and BEFORE mkdisk (which
# probe-ships libc++ into the image). Default flow skips this entirely.
if [ "${BUILD_LIBCXX:-0}" = "1" ]; then
    echo "=== Building libc++ (opt-in, --cxx) ==="
    bash build_script/build_libcxx.sh

    # The libc++ smoke ELF (user/test/libcxx_smoke.cpp, -DLIBCXX=1) is EXCLUDE_FROM_ALL,
    # so the main `ninja` above did not build it — and it could not have: its sysroot
    # libc++ headers/.so only exist after build_libcxx.sh ran. Build it now (ninja target
    # libcxx_smoke_elf), after libc++ is installed and before mkdisk's manifest check.
    echo "=== Building libc++ smoke ELF ==="
    ninja -C build libcxx_smoke_elf
fi

# 3. Generate disk.img (single disk, two partitions: ESP + root FAT32)
TEST="${TEST:-0}"
if echo "$CMAKE_EXTRA" | grep -q "TEST=1"; then
    TEST=1
fi
export TEST

./build_script/mkdisk.sh
