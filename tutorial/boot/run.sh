#!/bin/bash

if [ -z "$1" ]; then
  echo "need boot file!"
  exit 1
fi

echo "run boot $1"

CC=${HOME}/llvm-project/build/bin/clang
LD=${HOME}/llvm-project/build/bin/ld.lld

filename=$1
name="${filename%.*}"
extension="${filename##*.}"

if [ "$extension" = "asm" ]; then
  nasm -f bin $1 -o boot.bin
elif [ "$extension" = "s" ]; then
  obj=${name}.o
  ${CC} $1 -c -o ${obj}
  ${LD} ${obj} -Ttext 0x7c00 --oformat binary -o boot.bin
else
    echo "Unknown file type!"
    exit 1
fi

qemu-system-x86_64 boot.bin
