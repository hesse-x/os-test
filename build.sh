#!/bin/bash
# build.sh - CMake 编译内核 + EFI bootloader + 编译用户 ELF + 生成 disk.img + FAT32 启动映像
set -e

if [[ "$1" == "clear" ]]; then
    rm -rf build
    exit 0
fi

# 构建类型：默认 Release，-d 为 Debug（带 -g 调试信息）
BUILD_TYPE=Release
EXTRA_CFLAGS=""
if [[ "$1" == "-d" ]]; then
    BUILD_TYPE=Debug
    EXTRA_CFLAGS="-g -fno-omit-frame-pointer"
fi

# 1. CMake 编译
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../build_script/cmake/toolchain-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=$BUILD_TYPE ..
make
cd ..

# 用户态编译公共 flags
USER_CFLAGS="-m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector $EXTRA_CFLAGS"

# 2. 编译用户态进程
# disk_driver.elf (IOPL=3, I/O port access)
g++ $USER_CFLAGS -I. -c driver/disk_driver.cc -o build/disk_driver.o
objcopy --remove-section .note.gnu.property build/disk_driver.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/disk_driver.elf build/disk_driver.o

# kbd_driver.elf (IOPL=3, I/O port access)
g++ $USER_CFLAGS -I. -c driver/kbd_driver.cc -o build/kbd_driver.o
objcopy --remove-section .note.gnu.property build/kbd_driver.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/kbd_driver.elf build/kbd_driver.o

# kms_driver.elf (IOPL=0, framebuffer rendering)
g++ $USER_CFLAGS -I. -c driver/kms_driver.cc -o build/kms_driver.o
objcopy --remove-section .note.gnu.property build/kms_driver.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/kms_driver.elf build/kms_driver.o

# libc.a (static library: printf + FILE + string + malloc + _start)
g++ $USER_CFLAGS -I. -Iuser/include -c user/lib/stdio.cc -o build/stdio.o
g++ $USER_CFLAGS -I. -Iuser/include -c user/lib/string.cc -o build/string.o
g++ $USER_CFLAGS -I. -Iuser/include -c user/lib/start.cc -o build/start.o
g++ $USER_CFLAGS -I. -c user/lib/malloc.cc -o build/malloc.o
objcopy --remove-section .note.gnu.property build/stdio.o
objcopy --remove-section .note.gnu.property build/string.o
objcopy --remove-section .note.gnu.property build/start.o
objcopy --remove-section .note.gnu.property build/malloc.o
ar rcs build/libc.a build/start.o build/stdio.o build/string.o build/malloc.o

# shell.elf (IOPL=0, linked with libc.a)
g++ $USER_CFLAGS -I. -Iuser/include -c shell/shell.cc -o build/shell.o
objcopy --remove-section .note.gnu.property build/shell.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/shell.elf build/shell.o build/libc.a

# fs_driver.elf (IOPL=0)
g++ $USER_CFLAGS -I. -c driver/fs_driver.cc -o build/fs_driver.o
objcopy --remove-section .note.gnu.property build/fs_driver.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/fs_driver.elf build/fs_driver.o

# hello.elf (IOPL=0, C program linked with libc.a)
gcc $USER_CFLAGS -I. -Iuser/include -c user/hello.c -o build/hello.o
objcopy --remove-section .note.gnu.property build/hello.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/hello.elf build/hello.o build/libc.a

# malloctest.elf (IOPL=0, C program linked with libc.a)
gcc $USER_CFLAGS -I. -Iuser/include -c user/malloctest.c -o build/malloctest.o
objcopy --remove-section .note.gnu.property build/malloctest.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/malloctest.elf build/malloctest.o build/libc.a

# 3. 生成 disk.img
# LBA layout: 0=MBR, 1-100=disk_driver, 101-200=kbd_driver, 201-300=kms_driver, 301-400=shell, 401-500=fs_driver, 501+=FAT32
dd if=/dev/zero of=build/disk.img bs=512 count=4096

# Write ELFs to raw area (100 sectors = 50KB per slot)
dd if=build/disk_driver.elf of=build/disk.img bs=512 seek=1 conv=notrunc
dd if=build/kbd_driver.elf of=build/disk.img bs=512 seek=101 conv=notrunc
dd if=build/kms_driver.elf of=build/disk.img bs=512 seek=201 conv=notrunc
dd if=build/shell.elf of=build/disk.img bs=512 seek=301 conv=notrunc
dd if=build/fs_driver.elf of=build/disk.img bs=512 seek=401 conv=notrunc

# Create MBR partition table
# Partition 1: LBA 1-500, type 0xDA (non-FS data, for raw ELF storage)
# Partition 2: LBA 501-4095, type 0x0C (FAT32 LBA)
sfdisk build/disk.img <<EOF
label: dos
unit: sectors

build/disk.img1 : start=1, size=500, type=da
build/disk.img2 : start=501, size=3595, type=0c
EOF

# Extract FAT32 partition area, format it, add test files, write back
dd if=build/disk.img of=build/part2.img bs=512 skip=501 count=3595
mkfs.fat -F 32 -s 8 build/part2.img

# Create test files
echo "Hello from FAT32!" > build/hello.txt
echo "Microkernel OS with FAT32 support" > build/README

# Copy files into FAT32 image using mtools (no root needed)
mcopy -i build/part2.img build/hello.txt ::
mcopy -i build/part2.img build/README ::
mcopy -i build/part2.img build/hello.elf ::
mcopy -i build/part2.img build/malloctest.elf ::malloc.elf

# Write FAT32 partition back into disk.img
dd if=build/part2.img of=build/disk.img bs=512 seek=501 conv=notrunc

# Clean up temp files
rm -f build/part2.img build/hello.txt build/README

# 4. 构建 FAT32 启动映像
./mkimg.sh
