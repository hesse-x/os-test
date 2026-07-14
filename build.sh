#!/bin/bash
# build.sh - CMake builds kernel + EFI bootloader + userspace ELF + generates image
set -e

# Configure git hooks path so pre-push check works out of the box
git config core.hooksPath build_script/githooks

# Build type: default Release, -d for Debug (with -g debug info)
BUILD_TYPE=Release
CMAKE_EXTRA=""

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
        *)
            echo "Usage: $0 [-d] [--test] [--sanitizer] [--perf]"
            exit 1
            ;;
    esac
done

# Ensure SANITIZE is explicitly set so CMake cache doesn't retain stale values
if ! echo "$CMAKE_EXTRA" | grep -q "SANITIZE="; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DSANITIZE=0"
fi

# 1. CMake build (kernel + userspace)
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../build_script/cmake/toolchain-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
      $CMAKE_EXTRA \
      ..
make
cd ..

# 2. Publish sysroot artifacts (UAPI headers + libs → self-contained cross-target)
bash build_script/install-headers.sh
bash build_script/install-libs.sh

# 3. Generate disk.img (single disk, two partitions: ESP + root FAT32)
TEST="${TEST:-0}"
if echo "$CMAKE_EXTRA" | grep -q "TEST=1"; then
    TEST=1
fi
export TEST

./build_script/mkdisk.sh
