#!/bin/bash
# run.sh - QEMU boots the kernel
# Single-disk disk.img (two partitions: ESP + root FAT32), connected to q35 ICH9 SATA (IDE-compatible bus ide.0)
# UEFI boots from the ESP partition BOOTX64.EFI → loads myos.elf + init.elf
#
# Serial output is written directly to LOGFILE (default log.txt); serial input
# is not supported (RX path removed — see remove_serial_input.md). Keyboard input
# is injected via the QEMU monitor's `sendkey` command (monitor on stdio).
#
# -s: enable GDB remote debug (off by default)
# -o <file>: serial output written to the specified file (default log.txt)


LOGFILE="log.txt"
rm -f "$LOGFILE"

SERIAL_OPTS="-serial file:$LOGFILE -monitor stdio"

qemu-system-x86_64 \
    -machine q35 \
    -drive file=build/disk.img,format=raw,if=none,id=disk0 \
    -device ide-hd,drive=disk0,bus=ide.0 \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -vga none \
    -device virtio-gpu-pci \
    -m 512M -bios /usr/share/ovmf/OVMF.fd \
    -smp 2 \
    $SERIAL_OPTS \
    $@
