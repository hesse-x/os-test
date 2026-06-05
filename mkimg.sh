#!/bin/bash
# mkimg.sh - 创建 FAT32 启动映像
# BOOTX64.EFI 放到 \EFI\BOOT\，myos.elf 放到根目录
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
IMG_SIZE=128  # MB

if [ ! -f "${BUILD_DIR}/BOOTX64.EFI" ]; then
    echo "mkimg.sh: ${BUILD_DIR}/BOOTX64.EFI not found, run build.sh first"
    exit 1
fi

if [ ! -f "${BUILD_DIR}/myos.elf" ]; then
    echo "mkimg.sh: ${BUILD_DIR}/myos.elf not found, run build.sh first"
    exit 1
fi

# 创建零填充映像
dd if=/dev/zero of="${BUILD_DIR}/boot.img" bs=1M count=${IMG_SIZE} status=none

# 格式化为 FAT32（裸文件系统，无分区表）
mkfs.vfat -F 32 "${BUILD_DIR}/boot.img" >/dev/null

# 使用 mtools 复制文件（无需 root/sudo）
mmd -i "${BUILD_DIR}/boot.img" ::EFI
mmd -i "${BUILD_DIR}/boot.img" ::EFI/BOOT
mcopy -i "${BUILD_DIR}/boot.img" "${BUILD_DIR}/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI
mcopy -i "${BUILD_DIR}/boot.img" "${BUILD_DIR}/myos.elf" ::myos.elf
