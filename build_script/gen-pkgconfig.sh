#!/bin/bash
# gen-pkgconfig.sh — emit minimal .pc files for the Mesa meson cross-build.
#
# Mesa's meson discovers libdrm / zlib / expat via pkg-config (dependency('libdrm'),
# dependency('zlib', version: '>= 1.2.3'), dependency('expat')). install-libs.sh
# publishes the .so files into the sysroot but no .pc files exist anywhere, so
# pkg-config finds nothing. This script authors the four .pc files the Mesa build
# needs, pointing prefix/libdir/includedir at the sysroot so -I/-L land correctly.
#
# Run AFTER install-libs.sh + install-headers.sh (libs + headers must be in place).
# Idempotent: overwrites the pkgconfig dir each run.
#
# pkg-config module names (must match Mesa's dependency() calls):
#   libdrm  — dependency('libdrm', version: '>= 2.4.109')   (our libdrm = 2.4.134)
#   zlib    — dependency('zlib',   version: '>= 1.2.3')     (our zlib = 1.3.1)
#   expat   — dependency('expat')  (or find_library('expat'); the .pc covers both)
#   libffi  — dependency('ffi')    (Mesa virgl/softpipe don't strictly need it;
#               published anyway so any gallium GL-dispatch path resolves)
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$SRC/build}"
SYSROOT="${SYSROOT:-$BUILD/sysroot}"
PCDIR="$SYSROOT/usr/lib/pkgconfig"

echo "Generating pkg-config files → $PCDIR"
mkdir -p "$PCDIR"

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
Version: 2.4.109
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

echo "Generated:"
( cd "$PCDIR" && ls -la )
echo "Done. pkg-config files ready at $PCDIR"
