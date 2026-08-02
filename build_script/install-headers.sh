#!/bin/bash
# Publish the userspace SDK headers. musl owns the standard C/POSIX namespace;
# this repository only overlays XOS UAPI and genuinely OS-specific interfaces.

set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$SRC/build/sysroot/usr/include}"
CC="${CC:-clang}"

libcxx_backup=""
if [ -d "$DEST/c++" ]; then
    libcxx_backup="$(mktemp -d)"
    mv "$DEST/c++" "$libcxx_backup/c++"
fi

cleanup() {
    rm -f /tmp/xos-header-probe.c /tmp/xos-header-probe.log
    if [ -n "$libcxx_backup" ] && [ -d "$libcxx_backup/c++" ]; then
        mkdir -p "$DEST"
        mv "$libcxx_backup/c++" "$DEST/c++"
        rmdir "$libcxx_backup" 2>/dev/null || true
    fi
}
trap cleanup EXIT

rm -rf "$DEST"
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

build_dir="${BUILD:-$SRC/build}"
for header in ffi.h ffitarget.h fficonfig.h; do
    if [ -f "$build_dir/$header" ]; then
        cp "$build_dir/$header" "$DEST/$header"
    fi
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

if [ -n "$libcxx_backup" ] && [ -d "$libcxx_backup/c++" ]; then
    mv "$libcxx_backup/c++" "$DEST/c++"
    rmdir "$libcxx_backup" 2>/dev/null || true
    libcxx_backup=""
fi

echo "Headers installed successfully"
