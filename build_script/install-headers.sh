#!/bin/bash
# Publish the userspace SDK headers. musl owns the standard C/POSIX namespace;
# this repository only overlays XOS UAPI and genuinely OS-specific interfaces.

set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
FINAL_DEST="${1:-$SRC/build/sysroot/usr/include}"
DEST="$(mktemp -d "$SRC/build/.headers.XXXXXX")"
CC="${CC:-clang}"

cleanup() {
    rm -f /tmp/xos-header-probe.c /tmp/xos-header-probe.log
    rm -rf "$DEST"
}
trap cleanup EXIT

mkdir -p "$DEST/bits"

echo "Installing musl headers -> $DEST"

# Install the standard namespace from one source of truth. Generic arch bits
# are the fallback; x86_64 bits overwrite them where musl supplies an override.
cp -r "$SRC/third_party/musl/include/." "$DEST/"
cp -r "$SRC/third_party/musl/arch/generic/bits/." "$DEST/bits/"
cp -r "$SRC/third_party/musl/arch/x86_64/bits/." "$DEST/bits/"

# musl ships these two headers as templates. Generate exactly what musl's own
# build generates instead of maintaining stale checked-in copies.
sed -f "$SRC/third_party/musl/tools/mkalltypes.sed" \
    "$SRC/third_party/musl/arch/x86_64/bits/alltypes.h.in" \
    "$SRC/third_party/musl/include/alltypes.h.in" \
    > "$DEST/bits/alltypes.h"
cp "$SRC/third_party/musl/arch/x86_64/bits/syscall.h.in" \
    "$DEST/bits/syscall.h"
sed -n -e 's/__NR_/SYS_/p' \
    "$SRC/third_party/musl/arch/x86_64/bits/syscall.h.in" \
    >> "$DEST/bits/syscall.h"
printf '%s\n' '#define _POSIX_V6_LP64_OFF64 1' \
    '#define _POSIX_V7_LP64_OFF64 1' > "$DEST/bits/posix.h"
find "$DEST" -name '*.in' -delete

echo "Installing XOS headers"

# Kernel/userspace ABI contracts and Linux compatibility UAPI.
mkdir -p "$DEST/xos" "$DEST/linux" "$DEST/asm"
cp "$SRC"/include/uapi/xos/*.h "$DEST/xos/"
cp "$SRC"/include/uapi/linux/*.h "$DEST/linux/"
cp "$SRC"/include/uapi/asm/*.h "$DEST/asm/"
cp "$SRC"/user/include/xos/*.h "$DEST/xos/"

# Explicit OS-owned overlay. Never copy user/include wholesale: doing so used
# to publish source-tree forwarding shims and shadow fuller musl headers.
project_headers=(
    usb_hid.h
    linux/netlink.h
    sys/cdefs.h
    sys/device.h
    sys/ioccom.h
    sys/irq.h
    sys/pci.h
    sys/process.h
)
for header in "${project_headers[@]}"; do
    mkdir -p "$DEST/$(dirname "$header")"
    cp "$SRC/user/include/$header" "$DEST/$header"
done

# Public headers for libraries installed into the same SDK.
mkdir -p "$DEST/drm" "$DEST/libdrm"
cp "$SRC"/third_party/drm/include/drm/*.h "$DEST/drm/"
cp "$SRC"/third_party/drm/include/drm/*.h "$DEST/libdrm/"
cp "$SRC/third_party/drm/xf86drm.h" "$DEST/xf86drm.h"
cp "$SRC/third_party/drm/xf86drmMode.h" "$DEST/xf86drmMode.h"
cp "$SRC/third_party/drm/libsync.h" "$DEST/libsync.h"
cp "$SRC/third_party/libexpat/expat/lib/expat.h" "$DEST/expat.h"
cp "$SRC/third_party/libexpat/expat/lib/expat_external.h" \
    "$DEST/expat_external.h"
cp "$SRC/third_party/zlib/zlib.h" "$DEST/zlib.h"
cp "$SRC/third_party/zlib/zconf.h" "$DEST/zconf.h"

# Wayland's source headers and scanner-generated public protocol headers form
# one installed API. Mesa consumes these exclusively through the target sysroot.
wayland_src="$SRC/third_party/wayland/src"
wayland_egl="$SRC/third_party/wayland/egl"
wayland_cursor="$SRC/third_party/wayland/cursor"
for header in \
    wayland-util.h wayland-client.h wayland-client-core.h \
    wayland-server.h wayland-server-core.h; do
    cp "$wayland_src/$header" "$DEST/$header"
done
for header in wayland-egl.h wayland-egl-core.h wayland-egl-backend.h; do
    cp "$wayland_egl/$header" "$DEST/$header"
done
cp "$wayland_cursor/wayland-cursor.h" "$DEST/wayland-cursor.h"

build_dir="${BUILD:-$SRC/build}"
for header in ffi.h ffitarget.h fficonfig.h; do
    if [ -f "$build_dir/$header" ]; then
        cp "$build_dir/$header" "$DEST/$header"
    fi
done
for header in \
    wayland-version.h wayland-client-protocol.h wayland-server-protocol.h; do
    if [ ! -f "$build_dir/$header" ]; then
        echo "FAIL: generated Wayland header $build_dir/$header not found." >&2
        exit 1
    fi
    cp "$build_dir/$header" "$DEST/$header"
done

echo "Checking installed standard and XOS header closures"
probe_headers=(
    assert.h errno.h fcntl.h limits.h pthread.h signal.h stdio.h stdlib.h
    syscall.h termios.h time.h unistd.h sys/epoll.h sys/ioctl.h sys/ipc.h
    sys/mman.h sys/socket.h sys/stat.h sys/time.h xos/ipc.h xos/syscall.h
    xos/syscall_ext.h xos/unistd_ext.h
)
for header in "${probe_headers[@]}"; do
    printf '#include <%s>\n' "$header" > /tmp/xos-header-probe.c
    if ! "$CC" -nostdinc -ffreestanding -I"$DEST" -E \
        /tmp/xos-header-probe.c >/dev/null 2>/tmp/xos-header-probe.log; then
        echo "Header closure failed: <$header>" >&2
        sed -n '1,12p' /tmp/xos-header-probe.log >&2
        exit 1
    fi
done

# Preserve timestamps for unchanged headers so default incremental LLVM builds
# do not recompile thousands of objects. libc++ is installed by its own build.
mkdir -p "$FINAL_DEST"
rsync -r --checksum --delete --exclude 'c++/' "$DEST/" "$FINAL_DEST/"

echo "Headers installed successfully"
