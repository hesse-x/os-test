#!/bin/bash
# 启动QEMU，加载myos.iso镜像
#qemu-system-i386 -cdrom myos.iso
qemu-system-x86_64 -cdrom myos.iso -vga std -m 512M -bios /usr/share/ovmf/OVMF.fd
