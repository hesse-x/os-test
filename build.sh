#!/bin/bash
# build.sh - CMake 编译内核 + EFI bootloader + 构建 shell.elf + 生成 disk.img + FAT32 启动映像
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
    -I. -c shell/shell.cc -o build/shell.o
objcopy --remove-section .note.gnu.property build/shell.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/shell.elf build/shell.o

# 3. 生成 disk.img：LBA 0=MBR(空), LBA 1=disk_driver, LBA 33=kbd_driver, LBA 65=shell
dd if=/dev/zero of=build/disk.img bs=512 count=2048
dd if=build/disk_driver.elf of=build/disk.img bs=512 seek=1 conv=notrunc
dd if=build/kbd_driver.elf of=build/disk.img bs=512 seek=33 conv=notrunc
dd if=build/shell.elf of=build/disk.img bs=512 seek=65 conv=notrunc

# 4. 构建 FAT32 启动映像
./mkimg.sh
