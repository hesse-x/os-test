#!/bin/bash
# mkdisk.sh — 单盘两分区 disk.img 生成
#
# 布局:
#   LBA 0:           MBR + 分区表
#   分区1 (ESP):     FAT32, ~32MB — UEFI 引导文件
#     /EFI/BOOT/BOOTX64.EFI
#     /myos.elf
#     /init.elf          ← stub 加载到内存传给内核 (initrd-style)
#   分区2 (根):      FAT32, ~160MB — 根文件系统
#     /driver/kbd.dev
#     /usr/bin/{terminal,shell}
#     /usr/lib/libc.a
#     /lib/{libc.so,ld.so}
#     /local/{hello,hello_dyn}.elf
#     /README
#
# 内核从 boot_info 拿 init.elf 创建 init 进程, 不再需要裸 LBA slot。
# FAT32 驱动自己解析 MBR 分区表找根分区起始 LBA (fat32_init)。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
TESTDATA_DIR="${PROJECT_DIR}/testdata"

# 检查依赖文件
for f in init.elf myos.elf BOOTX64.EFI kbd_driver.elf terminal.elf shell.elf \
         libc.a libc.so hello.elf ldso.elf hello_dyn.elf; do
    if [ ! -f "${BUILD_DIR}/${f}" ]; then
        echo "mkdisk.sh: ${BUILD_DIR}/${f} not found, run build.sh first"
        exit 1
    fi
done

# 磁盘总大小: 192MB = 393216 扇区
DISK_SECTORS=$((192 * 1024 * 1024 / 512))
# 分区1 (ESP): 32MB = 65536 扇区, 从 LBA 2048 开始 (1MB 对齐)
PART1_START=2048
PART1_SECTORS=65536
# 分区2 (根): 剩余空间
PART2_START=$((PART1_START + PART1_SECTORS))
PART2_SECTORS=$((DISK_SECTORS - PART2_START))

# 创建零填充映像
dd if=/dev/zero of="${BUILD_DIR}/disk.img" bs=512 count=${DISK_SECTORS} status=none

# 创建 MBR 分区表
# 分区1: ESP, type 0xEF (EFI), 1MB 对齐 — OVMF 凭此类型识别 ESP
# 分区2: 根, type 0x0C (W95 FAT32 LBA)
sfdisk "${BUILD_DIR}/disk.img" <<EOF
label: dos
unit: sectors

${BUILD_DIR}/disk.img1 : start=${PART1_START}, size=${PART1_SECTORS}, type=ef
${BUILD_DIR}/disk.img2 : start=${PART2_START}, size=${PART2_SECTORS}, type=0c
EOF

# ===================== 分区1: ESP =====================
# 提取分区1 区域, 格式化, 写入引导文件, 写回
dd if="${BUILD_DIR}/disk.img" of="${BUILD_DIR}/part1.img" bs=512 skip=${PART1_START} count=${PART1_SECTORS} status=none
# FAT16: 32MB ESP 用 FAT32 簇数不足 (mtools WARNING + OVMF 拒绝)。
# UEFI 规范允许 FAT12/16/32, OVMF 对 FAT16 ESP 引导支持良好。
mkfs.fat -F 16 -n ESP "${BUILD_DIR}/part1.img" >/dev/null

mmd -i "${BUILD_DIR}/part1.img" ::EFI
mmd -i "${BUILD_DIR}/part1.img" ::EFI/BOOT
mcopy -i "${BUILD_DIR}/part1.img" "${BUILD_DIR}/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI
mcopy -i "${BUILD_DIR}/part1.img" "${BUILD_DIR}/myos.elf"     ::myos.elf
mcopy -i "${BUILD_DIR}/part1.img" "${BUILD_DIR}/init.elf"     ::init.elf

dd if="${BUILD_DIR}/part1.img" of="${BUILD_DIR}/disk.img" bs=512 seek=${PART1_START} conv=notrunc status=none
rm -f "${BUILD_DIR}/part1.img"

# ===================== 分区2: 根文件系统 =====================
dd if="${BUILD_DIR}/disk.img" of="${BUILD_DIR}/part2.img" bs=512 skip=${PART2_START} count=${PART2_SECTORS} status=none
mkfs.fat -F 32 -s 1 "${BUILD_DIR}/part2.img" >/dev/null
# 注: -s 1 (512B/簇) 在 64MB 下簇数达标 (新版 mtools 要求 ≥65525 簇)。

# 创建目录结构
mmd -i "${BUILD_DIR}/part2.img" ::driver
mmd -i "${BUILD_DIR}/part2.img" ::usr ::usr/bin ::usr/lib
mmd -i "${BUILD_DIR}/part2.img" ::local
mmd -i "${BUILD_DIR}/part2.img" ::lib

# 复制文件到目录结构
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/kbd_driver.elf"   ::driver/kbd.dev
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/terminal.elf"     ::usr/bin/terminal
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/shell.elf"        ::usr/bin/shell
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libc.a"           ::usr/lib/libc.a
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/hello.elf"        ::local/hello.elf
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/ldso.elf"         ::lib/ld.so
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libc.so"          ::lib/libc.so
# ld.so 多依赖测试 stub .so（plan_ld 阶段 D）：放 /test/lib/ 与生产 /lib/ 分区，
# ld.so load_one() 先试 /lib/ 失败回退 /test/lib/（对齐 Linux DT_RPATH 思路）
mmd -i "${BUILD_DIR}/part2.img" ::test ::test/lib
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/liba.so"          ::test/lib/liba.so
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libb.so"          ::test/lib/libb.so
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/hello_dyn.elf"    ::local/hello_dyn.elf

# Copy test ELFs to /test/ directory (test build only)
if [ "$TEST" = "1" ]; then
    for elf in test_runner.elf pipe.elf fcntl.elf string.elf malloc.elf \
               stdio.elf mmap.elf ipc.elf socket.elf process.elf \
               signal.elf poll.elf pci.elf ioctl.elf dev_vfs.elf \
               test_fpu.elf test_sse_smoke.elf pthread.elf \
               ld_test_single.elf ld_test_chain.elf \
               ld_test_diamond.elf ld_test_cycle.elf; do
        mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/${elf}" ::test/
    done
fi

# 保留根目录 README
mcopy -i "${BUILD_DIR}/part2.img" "${TESTDATA_DIR}/README" ::README

# 写回 FAT32 分区
dd if="${BUILD_DIR}/part2.img" of="${BUILD_DIR}/disk.img" bs=512 seek=${PART2_START} conv=notrunc status=none
rm -f "${BUILD_DIR}/part2.img"
