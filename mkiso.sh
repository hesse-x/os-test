#!/bin/bash
# mkiso.sh - 从 myos.bin 生成可引导 ISO
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
ISO_DIR="${BUILD_DIR}/iso"

mkdir -p "${ISO_DIR}/boot/grub"
cp "${BUILD_DIR}/myos.bin" "${ISO_DIR}/boot/myos.bin"
cp "${SCRIPT_DIR}/grub.cfg" "${ISO_DIR}/boot/grub/grub.cfg"
grub-mkrescue -o "${BUILD_DIR}/myos.iso" "${ISO_DIR}"
rm -rf "${ISO_DIR}"
