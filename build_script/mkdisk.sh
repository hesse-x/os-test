#!/bin/bash
# mkdisk.sh — 创建 disk.img（MBR + 裸 ELF 存储 + FAT32 分区）
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
TESTDATA_DIR="${PROJECT_DIR}/testdata"

# 检查依赖文件
for elf in disk_driver.elf kbd_driver.elf kms_driver.elf terminal.elf shell.elf fs_driver.elf; do
    if [ ! -f "${BUILD_DIR}/${elf}" ]; then
        echo "mkdisk.sh: ${BUILD_DIR}/${elf} not found, run build.sh first"
        exit 1
    fi
done

# 创建零填充映像 (16MB, 32768 扇区)
dd if=/dev/zero of="${BUILD_DIR}/disk.img" bs=512 count=32768 status=none

# 写入 ELF 到裸 LBA 区域 (每槽 100 扇区 = 50KB)
dd if="${BUILD_DIR}/disk_driver.elf"  of="${BUILD_DIR}/disk.img" bs=512 seek=1   conv=notrunc status=none
dd if="${BUILD_DIR}/kbd_driver.elf"  of="${BUILD_DIR}/disk.img" bs=512 seek=101 conv=notrunc status=none
dd if="${BUILD_DIR}/kms_driver.elf"  of="${BUILD_DIR}/disk.img" bs=512 seek=201 conv=notrunc status=none
dd if="${BUILD_DIR}/terminal.elf"    of="${BUILD_DIR}/disk.img" bs=512 seek=301 conv=notrunc status=none
dd if="${BUILD_DIR}/shell.elf"       of="${BUILD_DIR}/disk.img" bs=512 seek=401 conv=notrunc status=none
dd if="${BUILD_DIR}/fs_driver.elf"   of="${BUILD_DIR}/disk.img" bs=512 seek=501 conv=notrunc status=none

# 创建 MBR 分区表
sfdisk "${BUILD_DIR}/disk.img" <<EOF
label: dos
unit: sectors

${BUILD_DIR}/disk.img1 : start=1, size=600, type=da
${BUILD_DIR}/disk.img2 : start=601, size=32167, type=0c
EOF

# 提取 FAT32 分区区域，格式化，写入文件，写回
dd if="${BUILD_DIR}/disk.img" of="${BUILD_DIR}/part2.img" bs=512 skip=601 count=32167 status=none
mkfs.fat -F 32 -s 1 "${BUILD_DIR}/part2.img" >/dev/null

# 写入测试文件和用户程序
mcopy -i "${BUILD_DIR}/part2.img" "${TESTDATA_DIR}/README" ::
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/hello.elf" ::
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/malloc.elf" ::

# 写回 FAT32 分区
dd if="${BUILD_DIR}/part2.img" of="${BUILD_DIR}/disk.img" bs=512 seek=601 conv=notrunc status=none

# 清理临时文件
rm -f "${BUILD_DIR}/part2.img"
