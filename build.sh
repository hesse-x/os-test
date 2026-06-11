#!/bin/bash
# build.sh - CMake 编译内核 + EFI bootloader + 编译用户 ELF + 生成 disk.img + FAT32 启动映像
set -e

if [[ "$1" == "clear" ]]; then
    rm -rf build
    exit 0
fi

# 1. CMake 编译
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../build_script/cmake/toolchain-x86_64.cmake ..
make
cd ..

# 2. 编译用户态进程
# disk_driver.elf (IOPL=3, I/O port access)
g++ -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector \
    -I. -c driver/disk_driver.cc -o build/disk_driver.o
objcopy --remove-section .note.gnu.property build/disk_driver.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/disk_driver.elf build/disk_driver.o

# kbd_driver.elf (IOPL=3, I/O port access)
g++ -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector \
    -I. -c driver/kbd_driver.cc -o build/kbd_driver.o
objcopy --remove-section .note.gnu.property build/kbd_driver.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/kbd_driver.elf build/kbd_driver.o

# shell.elf (IOPL=0)
g++ -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector \
    -I. -c user/lib/malloc.cc -o build/malloc.o
g++ -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector \
    -I. -c shell/shell.cc -o build/shell.o
objcopy --remove-section .note.gnu.property build/shell.o
objcopy --remove-section .note.gnu.property build/malloc.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/shell.elf build/malloc.o build/shell.o

# fs_driver.elf (IOPL=0)
g++ -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector \
    -I. -c driver/fs_driver.cc -o build/fs_driver.o
objcopy --remove-section .note.gnu.property build/fs_driver.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/fs_driver.elf build/fs_driver.o

# 3. 生成 disk.img
# LBA layout: 0=MBR, 1-50=disk_driver, 51-100=kbd_driver, 101-150=shell, 151-200=fs_driver, 201+=FAT32
dd if=/dev/zero of=build/disk.img bs=512 count=2048

# Write ELFs to raw area (50 sectors = 25KB per slot)
dd if=build/disk_driver.elf of=build/disk.img bs=512 seek=1 conv=notrunc
dd if=build/kbd_driver.elf of=build/disk.img bs=512 seek=51 conv=notrunc
dd if=build/shell.elf of=build/disk.img bs=512 seek=101 conv=notrunc
dd if=build/fs_driver.elf of=build/disk.img bs=512 seek=151 conv=notrunc

# Create MBR partition table
# Partition 1: LBA 1-200, type 0xDA (non-FS data, for raw ELF storage)
# Partition 2: LBA 201-2047, type 0x0C (FAT32 LBA)
sfdisk build/disk.img <<EOF
label: dos
unit: sectors

build/disk.img1 : start=1, size=200, type=da
build/disk.img2 : start=201, size=1847, type=0c
EOF

# Extract FAT32 partition area, format it, add test files, write back
dd if=build/disk.img of=build/part2.img bs=512 skip=201 count=1847
mkfs.fat -F 32 -s 8 build/part2.img

# Create test files
echo "Hello from FAT32!" > build/hello.txt
echo "Microkernel OS with FAT32 support" > build/README

# Copy files into FAT32 image using mtools (no root needed)
mcopy -i build/part2.img build/hello.txt ::
mcopy -i build/part2.img build/README ::

# Write FAT32 partition back into disk.img
dd if=build/part2.img of=build/disk.img bs=512 seek=201 conv=notrunc

# Clean up temp files
rm -f build/part2.img build/hello.txt build/README

# 4. 构建 FAT32 启动映像
./mkimg.sh
