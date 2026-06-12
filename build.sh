#!/bin/bash
# build.sh - CMake 编译内核 + EFI bootloader + 用户态 ELF + 生成映像
set -e

# 构建类型：默认 Release，-d 为 Debug（带 -g 调试信息）
BUILD_TYPE=Release
if [[ "$1" == "-d" ]]; then
    BUILD_TYPE=Debug
fi

# 1. CMake 编译（内核 + 用户态）
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../build_script/cmake/toolchain-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=$BUILD_TYPE ..
make
cd ..

# 2. 生成 disk.img（裸 ELF + FAT32）
./build_script/mkdisk.sh

# 3. 生成 boot.img（EFI 启动盘）
./mkimg.sh
