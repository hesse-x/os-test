#!/bin/bash
# mkdisk.sh — 创建 disk.img（MBR + 裸 ELF 存储 + FAT32 分区）
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
TESTDATA_DIR="${PROJECT_DIR}/testdata"

# 检查依赖文件
for elf in fs_driver.elf init.elf; do
    if [ ! -f "${BUILD_DIR}/${elf}" ]; then
        echo "mkdisk.sh: ${BUILD_DIR}/${elf} not found, run build.sh first"
        exit 1
    fi
done

# 创建零填充映像 (64MB, 131072 扇区)
dd if=/dev/zero of="${BUILD_DIR}/disk.img" bs=512 count=131072 status=none

# 写入 ELF 到裸 LBA 区域 (每槽 100 扇区 = 50KB)
dd if="${BUILD_DIR}/fs_driver.elf"    of="${BUILD_DIR}/disk.img" bs=512 seek=101 conv=notrunc status=none
dd if="${BUILD_DIR}/init.elf"         of="${BUILD_DIR}/disk.img" bs=512 seek=201 conv=notrunc status=none

# 创建 MBR 分区表
sfdisk "${BUILD_DIR}/disk.img" <<EOF
label: dos
unit: sectors

${BUILD_DIR}/disk.img1 : start=1, size=300, type=da
${BUILD_DIR}/disk.img2 : start=301, size=130771, type=0c
EOF

# 提取 FAT32 分区区域，格式化，写入文件，写回
dd if="${BUILD_DIR}/disk.img" of="${BUILD_DIR}/part2.img" bs=512 skip=301 count=130771 status=none
mkfs.fat -F 32 -s 8 "${BUILD_DIR}/part2.img" >/dev/null

# 创建目录结构
mmd -i "${BUILD_DIR}/part2.img" ::driver
mmd -i "${BUILD_DIR}/part2.img" ::usr ::usr/bin ::usr/lib
mmd -i "${BUILD_DIR}/part2.img" ::local

# 复制文件到目录结构
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/kbd_driver.elf"   ::driver/kbd.dev
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/kms_driver.elf"   ::driver/kms.dev
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/terminal.elf"     ::usr/bin/terminal
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/shell.elf"        ::usr/bin/shell
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libc.a"           ::usr/lib/libc.a
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/hello.elf"        ::local/hello.elf
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/malloc.elf"       ::local/malloc.elf
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/pcie.elf"         ::local/pcie.elf

# 保留根目录 README
mcopy -i "${BUILD_DIR}/part2.img" "${TESTDATA_DIR}/README" ::README

# 写回 FAT32 分区
dd if="${BUILD_DIR}/part2.img" of="${BUILD_DIR}/disk.img" bs=512 seek=301 conv=notrunc status=none

# 清理临时文件
rm -f "${BUILD_DIR}/part2.img"
