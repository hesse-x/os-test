#!/bin/bash
# run.sh - QEMU 启动内核
# 单盘 disk.img (两分区: ESP + 根 FAT32), 接 q35 ICH9 SATA (IDE 兼容总线 ide.0)
# UEFI 从 ESP 分区引导 BOOTX64.EFI → 加载 myos.elf + init.elf
#
# 串口输出默认写入 log.txt，可通过 -o <file> 指定；串口输入需通过 socat 连接:
#   socat -,rawer UNIX-CONNECT:/tmp/qemu-serial.sock 2>&1 | tee -a <logfile>
# monitor 在 stdio（可输入 QEMU monitor 命令）
#
# -s: 启用 GDB 远程调试（默认关闭）
# -o <file>: 串口输出写入指定文件（默认 log.txt）

LOGFILE=log.txt

while getopts "o:" opt; do
    case $opt in
        o) LOGFILE="$OPTARG" ;;
    esac
done
shift $((OPTIND - 1))

rm -f /tmp/qemu-serial.sock "$LOGFILE"

SERIAL_OPTS="-chardev socket,id=s0,path=/tmp/qemu-serial.sock,server=on,wait=off,logfile=$LOGFILE -serial chardev:s0 -monitor stdio"

qemu-system-x86_64 \
    -machine q35 \
    -drive file=build/disk.img,format=raw,if=none,id=disk0 \
    -device ide-hd,drive=disk0,bus=ide.0 \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -vga none -device bochs-display -m 512M -bios /usr/share/ovmf/OVMF.fd \
    -smp 2 \
    $SERIAL_OPTS \
    "$@"
