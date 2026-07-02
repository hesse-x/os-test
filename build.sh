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
        --perf)
            CMAKE_EXTRA="$CMAKE_EXTRA -DPERF=1"
            shift
            ;;
        *)
            echo "Usage: $0 [-d] [--test] [--sanitizer] [--no-serial] [--perf]"
            exit 1
            ;;
    esac
done

# Ensure SANITIZE is explicitly set so CMake cache doesn't retain stale values
if ! echo "$CMAKE_EXTRA" | grep -q "SANITIZE="; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DSANITIZE=0"
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

# 2a. 提前检查裸 ELF 大小不超出 slot 容量。
# 裸 ELF 区 init.elf 在 LBA 101，slot = ELF_SLOT_SECTORS（与 kernel.c 对齐）。
# 超出时 dd 会越界写入 FAT32 分区，导致 .data/.bss 被覆盖，init 早期 page fault
#（典型症状：stdout 指针变 NULL/垃圾，fflush(stdout) 崩在 file_putc_internal）。
BARE_ELF_SLOT_SECTORS=2048  # = kernel.c ELF_SLOT_SECTORS，1MB
BARE_ELF_SLOT_BYTES=$((BARE_ELF_SLOT_SECTORS * 512))
INIT_SIZE=$(stat -c%s "${BUILD_DIR:-build}/init.elf" 2>/dev/null || echo 0)
if [ "$INIT_SIZE" -gt "$BARE_ELF_SLOT_BYTES" ]; then
    echo "build.sh: init.elf size ${INIT_SIZE} bytes exceeds bare-ELF slot capacity ${BARE_ELF_SLOT_BYTES} bytes (${BARE_ELF_SLOT_SECTORS} sectors)"
    echo "  init.elf is stored at LBA 101 in a raw ELF slot; FAT32 begins right after."
    echo "  If init.elf grows past the slot, dd overwrites the FAT32 partition and"
    echo "  clobbers init's .data — runtime symptoms look like a libc layout bug."
    echo "  Fix: shrink init.elf, or raise ELF_SLOT_SECTORS in kernel.c and the slot"
    echo "       size in build_script/mkdisk.sh (keep them in sync)."
    exit 1
fi

./build_script/mkdisk.sh

# 3. 生成 boot.img（EFI 启动盘）
./mkimg.sh
