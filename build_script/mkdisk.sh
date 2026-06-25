#!/bin/bash
# mkdisk.sh — 创建 disk.img（MBR + 裸 ELF 存储 + FAT32 分区）
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
TESTDATA_DIR="${PROJECT_DIR}/testdata"

# 检查依赖文件
if [ ! -f "${BUILD_DIR}/init.elf" ]; then
    echo "mkdisk.sh: ${BUILD_DIR}/init.elf not found, run build.sh first"
    exit 1
fi

# 创建零填充映像 (64MB, 131072 扇区)
# 注意: -F 32 -s 8 (4KB/簇) 在 64MB 下簇数不足 65525 下限，新版 mtools 拒绝操作。
# 改用 -s 1 (512B/簇) 确保簇数达标。
DISK_SECTORS=$((64 * 1024 * 1024 / 512))      # 131072
PART2_SECTORS=$((DISK_SECTORS - 201))           # FAT32 分区扇区数
dd if=/dev/zero of="${BUILD_DIR}/disk.img" bs=512 count=${DISK_SECTORS} status=none

# 写入 ELF 到裸 LBA 区域 (init: LBA 101, fs_driver removed)
dd if="${BUILD_DIR}/init.elf"         of="${BUILD_DIR}/disk.img" bs=512 seek=101 conv=notrunc status=none

# 创建 MBR 分区表
# 分区1: LBA 1-200 (裸 ELF: init at LBA 101), 分区2: LBA 201+ (FAT32)
sfdisk "${BUILD_DIR}/disk.img" <<EOF
label: dos
unit: sectors

${BUILD_DIR}/disk.img1 : start=1, size=200, type=da
${BUILD_DIR}/disk.img2 : start=201, size=${PART2_SECTORS}, type=0c
EOF

# 提取 FAT32 分区区域，格式化，写入文件，写回
dd if="${BUILD_DIR}/disk.img" of="${BUILD_DIR}/part2.img" bs=512 skip=201 count=${PART2_SECTORS} status=none
mkfs.fat -F 32 -s 1 "${BUILD_DIR}/part2.img" >/dev/null

# 创建目录结构
mmd -i "${BUILD_DIR}/part2.img" ::driver
mmd -i "${BUILD_DIR}/part2.img" ::usr ::usr/bin ::usr/lib
mmd -i "${BUILD_DIR}/part2.img" ::local

# 复制文件到目录结构
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/kbd_driver.elf"   ::driver/kbd.dev
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/terminal.elf"     ::usr/bin/terminal
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/shell.elf"        ::usr/bin/shell
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libc.a"           ::usr/lib/libc.a
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/hello.elf"        ::local/hello.elf

# Copy test ELFs to /test/ directory (test build only)
if [ "$TEST" = "1" ]; then
    mmd -i "${BUILD_DIR}/part2.img" ::test
    for elf in test_runner.elf pipe.elf fcntl.elf string.elf malloc.elf \
               stdio.elf mmap.elf ipc.elf socket.elf process.elf \
               signal.elf poll.elf pci.elf ioctl.elf dev_vfs.elf; do
        mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/${elf}" ::test/
    done
fi

# 保留根目录 README
mcopy -i "${BUILD_DIR}/part2.img" "${TESTDATA_DIR}/README" ::README

# 写回 FAT32 分区
dd if="${BUILD_DIR}/part2.img" of="${BUILD_DIR}/disk.img" bs=512 seek=201 conv=notrunc status=none

# 清理临时文件
rm -f "${BUILD_DIR}/part2.img"
