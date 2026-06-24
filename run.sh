#!/bin/bash
# run.sh - QEMU 启动内核
# disk.img 在 ICH9 port 0，boot.img 在 port 1
# UEFI 从 port 1 的 FAT32 引导 BOOTX64.EFI → 加载 myos.elf
#
# 串口输出始终写入 log.txt，串口输入需通过 socat 连接:
#   socat -,rawer UNIX-CONNECT:/tmp/qemu-serial.sock 2>&1 | tee -a log.txt
# monitor 在 stdio（可输入 QEMU monitor 命令）
#
# -s: 启用 GDB 远程调试（默认关闭）

rm -f /tmp/qemu-serial.sock log.txt

SERIAL_OPTS="-chardev socket,id=s0,path=/tmp/qemu-serial.sock,server=on,wait=off,logfile=log.txt -serial chardev:s0 -monitor stdio"

# qemu-system-x86_64 \
~/opensource/qemu/build/qemu-system-x86_64 \
    -machine q35 \
    -drive file=build/boot.img,format=raw,if=none,id=boot0 \
    -device ide-hd,drive=boot0,bus=ide.0 \
    -drive file=build/disk.img,format=raw,if=none,id=disk0 \
    -device ide-hd,drive=disk0,bus=ide.1 \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -vga none -device bochs-display -m 512M -bios /usr/share/ovmf/OVMF.fd \
    -smp 2 \
    $SERIAL_OPTS \
    "$@"
