#!/bin/bash
# mkdisk.sh — generate single-disk two-partition disk.img
#
# Layout:
#   LBA 0:           MBR + partition table
#   Partition 1 (ESP): FAT32, ~32MB — UEFI boot files
#     /EFI/BOOT/BOOTX64.EFI
#     /myos.elf
#     /init.elf          ← stub loads into memory and passes to kernel (initrd-style)
#   Partition 2 (root):  FAT32, ~160MB — root file system
#     /driver/kbd.dev
#     /usr/bin/{terminal,shell}
#     /usr/lib/libc.a
#     /lib/{libc.so,libinput.so,libm.so,libdrm.so,ld.so}
#     /local/{hello,hello_dyn}.elf
#     /test/drm_test.elf          ← only copied in test builds
#     /README
#
# The kernel gets init.elf from boot_info to create the init process, no longer needs a raw LBA slot.
# The FAT32 driver parses the MBR partition table itself to find the root partition start LBA (fat32_init).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
TESTDATA_DIR="${PROJECT_DIR}/testdata"

# Check dependency files
for f in init.elf myos.elf BOOTX64.EFI evdev.elf terminal.elf shell.elf \
         libc.a libc.so libinput.so libudev.so libm.so libdrm.so hello.elf ldso.elf hello_dyn.elf udevd.elf; do
    if [ ! -f "${BUILD_DIR}/${f}" ]; then
        echo "mkdisk.sh: ${BUILD_DIR}/${f} not found, run build.sh first"
        exit 1
    fi
done

# Total disk size: 192MB = 393216 sectors
DISK_SECTORS=$((192 * 1024 * 1024 / 512))
# Partition 1 (ESP): 32MB = 65536 sectors, starts at LBA 2048 (1MB alignment)
PART1_START=2048
PART1_SECTORS=65536
# Partition 2 (root): remaining space
PART2_START=$((PART1_START + PART1_SECTORS))
PART2_SECTORS=$((DISK_SECTORS - PART2_START))

# Create zero-filled image
dd if=/dev/zero of="${BUILD_DIR}/disk.img" bs=512 count=${DISK_SECTORS} status=none

# Create MBR partition table
# Partition 1: ESP, type 0xEF (EFI), 1MB aligned — OVMF recognizes ESP by this type
# Partition 2: root, type 0x0C (W95 FAT32 LBA)
sfdisk "${BUILD_DIR}/disk.img" <<EOF
label: dos
unit: sectors

${BUILD_DIR}/disk.img1 : start=${PART1_START}, size=${PART1_SECTORS}, type=ef
${BUILD_DIR}/disk.img2 : start=${PART2_START}, size=${PART2_SECTORS}, type=0c
EOF

# ===================== Partition 1: ESP =====================
# Extract partition 1 region, format, write boot files, write back
dd if="${BUILD_DIR}/disk.img" of="${BUILD_DIR}/part1.img" bs=512 skip=${PART1_START} count=${PART1_SECTORS} status=none
# FAT16: a 32MB ESP has too few FAT32 clusters (mtools WARNING + OVMF refuses).
# UEFI spec allows FAT12/16/32; OVMF boots FAT16 ESP well.
mkfs.fat -F 16 -n ESP "${BUILD_DIR}/part1.img" >/dev/null

mmd -i "${BUILD_DIR}/part1.img" ::EFI
mmd -i "${BUILD_DIR}/part1.img" ::EFI/BOOT
mcopy -i "${BUILD_DIR}/part1.img" "${BUILD_DIR}/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI
mcopy -i "${BUILD_DIR}/part1.img" "${BUILD_DIR}/myos.elf"     ::myos.elf
mcopy -i "${BUILD_DIR}/part1.img" "${BUILD_DIR}/init.elf"     ::init.elf

dd if="${BUILD_DIR}/part1.img" of="${BUILD_DIR}/disk.img" bs=512 seek=${PART1_START} conv=notrunc status=none
rm -f "${BUILD_DIR}/part1.img"

# ===================== Partition 2: root file system =====================
dd if="${BUILD_DIR}/disk.img" of="${BUILD_DIR}/part2.img" bs=512 skip=${PART2_START} count=${PART2_SECTORS} status=none
mkfs.fat -F 32 -s 1 "${BUILD_DIR}/part2.img" >/dev/null
# Note: -s 1 (512B/cluster) at 64MB yields enough clusters (newer mtools requires ≥65525 clusters).

# Create directory structure
mmd -i "${BUILD_DIR}/part2.img" ::driver
mmd -i "${BUILD_DIR}/part2.img" ::usr ::usr/bin ::usr/lib
mmd -i "${BUILD_DIR}/part2.img" ::local
mmd -i "${BUILD_DIR}/part2.img" ::lib
mmd -i "${BUILD_DIR}/part2.img" ::usr/share ::usr/share/libinput

# Copy files into directory structure
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/evdev.elf"        ::driver/evdev.dev
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/terminal.elf"     ::usr/bin/terminal
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/shell.elf"        ::usr/bin/shell
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/udevd.elf"        ::usr/bin/udevd
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libc.a"           ::usr/lib/libc.a
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/hello.elf"        ::local/hello.elf
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/ldso.elf"         ::lib/ld.so
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libc.so"          ::lib/libc.so
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libinput.so"     ::lib/libinput.so
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libudev.so"      ::lib/libudev.so
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libm.so"        ::lib/libm.so
mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/libdrm.so"     ::lib/libdrm.so
# ld.so multi-dependency test stub .so (plan_ld phase D): placed in /test/lib/ separate from production /lib/,
# ld.so load_one() tries /lib/ first and falls back to /test/lib/ on failure (mirrors Linux DT_RPATH idea)
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
               ld_test_diamond.elf ld_test_cycle.elf \
               drm_test.elf drm_ioctl.elf drm_phase_c.elf ioctl_varlen.elf \
               epoll.elf eventfd.elf timerfd.elf signalfd.elf mount.elf \
               test_sysfs.elf test_libudev.elf drm_test_link.elf \
               test_vfs_dispatch.elf test_inode_refcount.elf test_tmpfs_socket.elf \
               test_rename.elf test_udevd_db.elf test_udevd.elf test_dev_vfs_dynamic.elf \
               test_mprotect.elf; do
        mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/${elf}" ::test/
    done
fi

# Copy libinput quirks
mcopy -i "${BUILD_DIR}/part2.img" "${PROJECT_DIR}/third_party/libinput/quirks/10-generic-keyboard.quirks"  ::usr/share/libinput/
mcopy -i "${BUILD_DIR}/part2.img" "${PROJECT_DIR}/third_party/libinput/quirks/10-generic-mouse.quirks"     ::usr/share/libinput/

# Preserve root directory README
mcopy -i "${BUILD_DIR}/part2.img" "${TESTDATA_DIR}/README" ::README

# Write back FAT32 partition
dd if="${BUILD_DIR}/part2.img" of="${BUILD_DIR}/disk.img" bs=512 seek=${PART2_START} conv=notrunc status=none
rm -f "${BUILD_DIR}/part2.img"
