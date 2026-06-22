#!/bin/bash
# run.sh - QEMU 启动内核
# disk.img 在 ICH9 port 0，boot.img 在 port 1
# UEFI 从 port 1 的 FAT32 引导 BOOTX64.EFI → 加载 myos.elf
#
# 默认: -serial mon:stdio（串口+monitor 合并在终端）
# --log-serial: 串口走 Unix socket (/tmp/qemu-serial.sock)，
#   monitor 在 stdio。另开 tmux pane 连接:
#     socat -,rawer UNIX-CONNECT:/tmp/qemu-serial.sock 2>&1 | tee log.txt
#   即可交互 + 日志。纯日志不显示: 去掉 tee，用 > log.txt
#
# -s: 启用 GDB 远程调试（默认关闭）

LOG_SERIAL=0
for arg in "$@"; do
    case $arg in
        --log-serial) LOG_SERIAL=1; shift;;
    esac
done

SERIAL_OPTS="-serial mon:stdio"

if [ $LOG_SERIAL -eq 1 ]; then
    rm -f /tmp/qemu-serial.sock
    SERIAL_OPTS="-chardev socket,id=s0,path=/tmp/qemu-serial.sock,server=on,wait=off -serial chardev:s0 -monitor stdio"
fi

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
    -vga std -m 512M -bios /usr/share/ovmf/OVMF.fd \
    -smp 2 \
    $SERIAL_OPTS \
    "$@"
