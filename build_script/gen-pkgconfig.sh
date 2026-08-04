#!/bin/bash
# gen-pkgconfig.sh — emit minimal .pc files for the Mesa meson cross-build.
#
# Mesa's meson discovers its target libraries and Wayland protocol data via
# pkg-config. install-libs.sh publishes the .so files into the sysroot but the
# hand-written CMake adapters do not produce .pc files, so this script supplies
# that metadata and stages the data-only wayland-protocols package.
#
# Run AFTER install-libs.sh + install-headers.sh (libs + headers must be in place).
# Idempotent: overwrites the pkgconfig dir each run.
#
# pkg-config module names (must match Mesa's dependency() calls):
#   libdrm  — dependency('libdrm', version: '>= 2.4.109')   (our libdrm = 2.4.134)
#   zlib    — dependency('zlib',   version: '>= 1.2.3')     (our zlib = 1.3.1)
#   expat   — dependency('expat')  (or find_library('expat'); the .pc covers both)
#   libffi  — dependency('ffi')
#   wayland-client / wayland-server / wayland-egl-backend / wayland-protocols
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$SRC/build}"
SYSROOT="${SYSROOT:-$BUILD/sysroot}"
PCDIR="$SYSROOT/usr/lib/pkgconfig"
DATAPCDIR="$SYSROOT/usr/share/pkgconfig"
WAYLAND_PROTOCOLS_DIR="$SYSROOT/usr/share/wayland-protocols"

echo "Generating pkg-config files → $PCDIR"
mkdir -p "$PCDIR" "$DATAPCDIR"

# prefix is the sysroot itself so ${libdir}/${includedir} resolve to the actual
# .so / header locations. Mesa links with these via the cross-file's sys_root, so
# the -L/-I here are absolute sysroot paths (no relocation needed).
cat > "$PCDIR/libdrm.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/usr/lib
includedir=\${prefix}/usr/include

Name: libdrm
Description: Userspace interface to kernel DRM services (freestanding OS port)
Version: 2.4.134
Libs: -L\${libdir} -ldrm
# -I\${includedir}/libdrm mirrors upstream libdrm.pc.in: xf86drm.h does
# `#include <drm.h>` (top-level style) while the UAPI headers ship under libdrm/
# (install-headers.sh step 5, matching third_party/drm/meson.build:303-314).
Cflags: -I\${includedir} -I\${includedir}/libdrm
EOF

cat > "$PCDIR/zlib.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/usr/lib
includedir=\${prefix}/usr/include

Name: zlib
Description: zlib compression library (freestanding OS port)
Version: 1.2.3
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF

cat > "$PCDIR/expat.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/usr/lib
includedir=\${prefix}/usr/include

Name: expat
Description: expat XML parser library (freestanding OS port)
Version: 2.8.2
Libs: -L\${libdir} -lexpat
Cflags: -I\${includedir}
EOF

cat > "$PCDIR/libffi.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/usr/lib
includedir=\${prefix}/usr/include

Name: libffi
Description: libffi foreign function interface library (freestanding OS port)
Version: 3.2.1
Libs: -L\${libdir} -lffi
Cflags: -I\${includedir}
EOF

cat > "$PCDIR/wayland-client.pc" <<EOF
prefix=/usr
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: Wayland Client
Description: Wayland client library (freestanding OS port)
Version: 1.25.91
Libs: -L\${libdir} -lwayland-client
Cflags: -I\${includedir}
EOF

cat > "$PCDIR/wayland-server.pc" <<EOF
prefix=/usr
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: Wayland Server
Description: Wayland server library (freestanding OS port)
Version: 1.25.91
Libs: -L\${libdir} -lwayland-server
Cflags: -I\${includedir}
EOF

# Mesa uses this package only to locate wayland-egl-backend.h; upstream Wayland
# intentionally defines it as a header-only pkg-config dependency.
cat > "$PCDIR/wayland-egl-backend.pc" <<EOF
prefix=/usr
includedir=\${prefix}/include

Name: wayland-egl-backend
Description: Backend wayland-egl interface
Version: 3
Cflags: -I\${includedir}
EOF

# Keep protocol XML inside the target sysroot. pc_sysrootdir makes pkg-config
# return the staged host-visible path while retaining the conventional /usr
# prefix in the metadata.
rm -rf "$WAYLAND_PROTOCOLS_DIR"
mkdir -p "$WAYLAND_PROTOCOLS_DIR"
cp -R "$SRC/third_party/wayland-protocols/stable" \
      "$SRC/third_party/wayland-protocols/staging" \
      "$SRC/third_party/wayland-protocols/unstable" \
      "$WAYLAND_PROTOCOLS_DIR/"
cat > "$DATAPCDIR/wayland-protocols.pc" <<EOF
prefix=/usr
datarootdir=\${prefix}/share
pkgdatadir=\${pc_sysrootdir}\${datarootdir}/wayland-protocols

Name: Wayland Protocols
Description: Wayland protocol files
Version: 1.49
EOF

echo "Generated:"
( cd "$PCDIR" && ls -la )
( cd "$DATAPCDIR" && ls -la )
echo "Done. pkg-config files ready at $PCDIR"
