#!/bin/bash
# build.sh - CMake 编译内核 + EFI bootloader + 用户态 ELF + 生成映像
set -e

# 构建类型：默认 Release，-d 为 Debug（带 -g 调试信息）
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
        --no-serial)
            CMAKE_EXTRA="$CMAKE_EXTRA -DNSERIAL"
            shift
            ;;
        *)
            echo "Usage: $0 [-d] [--test] [--sanitizer] [--no-serial]"
            exit 1
            ;;
    esac
done

# Ensure SANITIZE is explicitly set so CMake cache doesn't retain stale values
if ! echo "$CMAKE_EXTRA" | grep -q "SANITIZE="; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DSANITIZE=0"
fi

# Same for TEST — without an explicit -DTEST=0, CMake would reuse the cached
# value from a previous --test build, silently keeping the shell's #ifdef TEST
# branch (auto-running perf) even under a plain ./build.sh.
if ! echo "$CMAKE_EXTRA" | grep -q "TEST="; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DTEST=0"
fi

# 1. CMake 编译（内核 + 用户态）
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../build_script/cmake/toolchain-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
      $CMAKE_EXTRA \
      ..
make
cd ..

# 2. 生成 disk.img（裸 ELF + FAT32）
TEST="${TEST:-0}"
if echo "$CMAKE_EXTRA" | grep -q "TEST=1"; then
    TEST=1
fi
export TEST
./build_script/mkdisk.sh

# 3. 生成 boot.img（EFI 启动盘）
./mkimg.sh
