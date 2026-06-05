#!/bin/bash
# build.sh - CMake 编译内核 + 构建 shell.elf + 生成 disk.img + 打包 ISO
set -e

if [[ "$1" == "clear" ]]; then
    rm -rf build
    exit 0
fi

# 1. CMake 编译内核
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-i686.cmake ..
make
cd ..

# 2. 编译用户态 shell.cc → shell.elf（ELF32 static binary, 入口 0x400000）
g++ -m32 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector \
    -c user/shell.cc -o build/shell.o
i686-linux-gnu-ld -Ttext 0x400000 -o build/shell.elf build/shell.o

# 3. 生成 disk.img：LBA 0 = MBR(空), LBA 1+ = shell.elf
dd if=/dev/zero of=build/disk.img bs=512 count=2048
dd if=build/shell.elf of=build/disk.img bs=512 seek=1 conv=notrunc

# 4. 打包 ISO
./mkiso.sh
