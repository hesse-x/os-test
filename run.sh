#!/bin/bash
# run.sh - QEMU 启动内核
# -drive boot.img: UEFI 从 FAT32 映像引导 BOOTX64.EFI → 加载 myos.elf
# -drive disk.img: ATA PIO，LBA 1 起存放 shell.elf
# -serial: 串口输出到 log.txt
# 注释掉的 -s -S 用于 GDB 远程调试

qemu-system-x86_64 \
    -drive file=build/boot.img,format=raw \
    -vga std -m 512M -bios /usr/share/ovmf/OVMF.fd \
    -serial file:log.txt \
    -drive file=build/disk.img,format=raw,if=ide

# qemu-system-x86_64 -drive file=build/boot.img,format=raw -vga std -m 512M -bios /usr/share/ovmf/OVMF.fd -serial file:log.txt -drive file=build/disk.img,format=raw,if=ide -s -S
# gdb -ex "target remote localhost:1234"
