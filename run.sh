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

# virgl GL backend: QEMU 8.2.2 has no virtio-gpu-pci.gl property — virgl 3D is
# driven by the display backend. -display egl-headless gives virtio-gpu an EGL
# GL context (no GUI window), so the host advertises virgl capsets 1/2 and the
# kernel caches them. This run is headless/serial-only (-vga none, monitor on
# stdio), so egl-headless rather than gtk,gl=on. Then /dev/dri/renderD128
# GETPARAM(SUPPORTED_CAPSET_IDs) reports VIRGL/VIRGL2, and the virgl_channel
# host-dependent cases run instead of self-skipping.
qemu-system-x86_64 \
    -machine q35 \
    --enable-kvm -cpu host \
    -drive file=build/disk.img,format=raw,if=none,id=disk0 \
    -device ide-hd,drive=disk0,bus=ide.0 \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -vga none \
    -device virtio-gpu-gl \
    -display sdl,gl=on \
    -m 2G -bios /usr/share/ovmf/OVMF.fd \
    -smp 2 \
    $SERIAL_OPTS \
    $@
