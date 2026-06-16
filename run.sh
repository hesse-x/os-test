#!/bin/bash
# run.sh - QEMU 启动内核
# disk.img 在 ICH9 port 0，boot.img 在 port 1
# UEFI 从 port 1 的 FAT32 引导 BOOTX64.EFI → 加载 myos.elf
# -serial: 串口输出到 log.txt
# 注释掉的 -s -S 用于 GDB 远程调试

qemu-system-x86_64 \
    -machine q35 \
    -drive file=build/disk.img,format=raw,if=none,id=disk0 \
    -device ide-hd,drive=disk0,bus=ide.0 \
    -drive file=build/boot.img,format=raw,if=none,id=boot0 \
    -device ide-hd,drive=boot0,bus=ide.1 \
    -vga std -m 512M -bios /usr/share/ovmf/OVMF.fd \
    -smp 2 \
    -serial file:log.txt

# qemu-system-x86_64 -machine q35 -drive file=build/disk.img,format=raw,if=none,id=disk0 -device ide-hd,drive=disk0,bus=ide.0 -drive file=build/boot.img,format=raw,if=none,id=boot0 -device ide-hd,drive=boot0,bus=ide.1 -vga std -m 512M -bios /usr/share/ovmf/OVMF.fd -serial file:log.txt -smp 2 -s -S
# gdb -ex "target remote localhost:1234"
