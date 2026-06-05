#!/bin/bash
# build.sh - CMake 编译内核 + EFI bootloader + 构建 shell.elf + 生成 disk.img + FAT32 启动映像
set -e

if [[ "$1" == "clear" ]]; then
    rm -rf build
    exit 0
fi

# 1. CMake 编译
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-x86_64.cmake ..
make
cd ..

# 2. 编译用户态 shell.cc → shell.elf
g++ -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector \
    -c user/shell.cc -o build/shell.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/shell.elf build/shell.o

# 3. 生成 disk.img：LBA 0 = MBR(空), LBA 1+ = shell.elf
dd if=/dev/zero of=build/disk.img bs=512 count=2048
dd if=build/shell.elf of=build/disk.img bs=512 seek=1 conv=notrunc

# 4. 构建 FAT32 启动映像
./mkimg.sh
