#!/bin/bash

if [ -z "$1" ]; then
  echo "need boot file!"
  exit 1
fi

echo "run boot $1"

nasm -f bin $1 -o boot.bin

qemu-system-x86_64 boot.bin
