#!/bin/bash
# Cross-build matching Vulkan headers and loader into the target sysroot.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
SYSROOT="$ROOT/build/sysroot"
HEADERS_BUILD="$ROOT/build/vulkan-headers"
LOADER_BUILD="$ROOT/build/vulkan-loader"
TOOLCHAIN="$ROOT/build_script/cmake/toolchain-llvm-x86_64.cmake"

header_version=$(awk '/^#define VK_HEADER_VERSION / { print $3; exit }' \
    "$ROOT/third_party/Vulkan-Headers/include/vulkan/vulkan_core.h")
if [ "$header_version" != "354" ]; then
    echo "ERROR: expected VK_HEADER_VERSION 354, got $header_version" >&2
    exit 1
fi

cmake -S "$ROOT/third_party/Vulkan-Headers" -B "$HEADERS_BUILD" -G Ninja \
    -DCMAKE_INSTALL_PREFIX=/usr -DVULKAN_HEADERS_ENABLE_TESTS=OFF
DESTDIR="$SYSROOT" cmake --install "$HEADERS_BUILD"

PKG_CONFIG_SYSROOT_DIR="$SYSROOT" \
PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig" \
cmake -S "$ROOT/third_party/Vulkan-Loader" -B "$LOADER_BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib -DVULKAN_HEADERS_INSTALL_DIR="$SYSROOT/usr" \
    -DBUILD_TESTS=OFF -DBUILD_WSI_XCB_SUPPORT=OFF \
    -DBUILD_WSI_XLIB_SUPPORT=OFF -DBUILD_WSI_XLIB_XRANDR_SUPPORT=OFF \
    -DBUILD_WSI_WAYLAND_SUPPORT=ON -DBUILD_WSI_DIRECTFB_SUPPORT=OFF
cmake --build "$LOADER_BUILD" --target vulkan
DESTDIR="$SYSROOT" cmake --install "$LOADER_BUILD" --strip

real_loader=$(find "$SYSROOT/usr/lib" -maxdepth 1 -type f -name 'libvulkan.so.*' | sort -V | tail -1)
if [ -z "$real_loader" ]; then
    echo "ERROR: Vulkan Loader installation produced no real shared library" >&2
    exit 1
fi
for name in libvulkan.so libvulkan.so.1; do
    cp -f "$real_loader" "$ROOT/build/$name"
done

echo "Vulkan headers/loader: header=$header_version, loader=$(git -C "$ROOT/third_party/Vulkan-Loader" rev-parse --short HEAD)"
