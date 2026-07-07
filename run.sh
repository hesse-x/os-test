#!/bin/bash
# run.sh - QEMU boots the kernel
# Single-disk disk.img (two partitions: ESP + root FAT32), connected to q35 ICH9 SATA (IDE-compatible bus ide.0)
# UEFI boots from the ESP partition BOOTX64.EFI → loads myos.elf + init.elf
#
# Serial output is written to log.txt by default, can be specified via -o <file>; serial input requires socat connection:
#   socat -,rawer UNIX-CONNECT:/tmp/qemu-serial.sock 2>&1 | tee -a <logfile>
# monitor on stdio (can input QEMU monitor commands)
#
# -s: enable GDB remote debug (off by default)
# -o <file>: serial output written to the specified file (default log.txt)


while getopts "o:" opt; do
    case $opt in
        o) LOGFILE="$OPTARG" ;;
    esac
done
shift $((OPTIND - 1))

LOGFILE=log.txt
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
    $@
