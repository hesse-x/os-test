#!/bin/bash
# Publish existing CMake/Mesa products and metadata for the wlroots Meson build.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD="$ROOT/build"
SYSROOT="$BUILD/sysroot"
INCLUDE_DIR="$SYSROOT/usr/include"
LIB_DIR="$SYSROOT/usr/lib"
PC_DIR="$LIB_DIR/pkgconfig"
NATIVE_PC_DIR="$BUILD/wlroots/native-pkgconfig"
NATIVE_FILE="$BUILD/wlroots-native.txt"
PROTOCOL_DIR="$SYSROOT/usr/share/wayland-protocols"
PROTOCOL_HEADER_BUILD="$BUILD/wlroots/wayland-protocols/include/wayland-protocols"

install -d "$INCLUDE_DIR" "$LIB_DIR" "$PC_DIR" "$NATIVE_PC_DIR"
install -d "$PROTOCOL_DIR"
cp -a "$ROOT/third_party/wayland-protocols/." "$PROTOCOL_DIR/"
if [[ -d "$PROTOCOL_HEADER_BUILD" ]]; then
	install -d "$INCLUDE_DIR/wayland-protocols"
	install -m 644 "$PROTOCOL_HEADER_BUILD/"*.h \
		"$INCLUDE_DIR/wayland-protocols/"
fi

install -m 644 "$ROOT/third_party/wayland/src/wayland-server-core.h" "$INCLUDE_DIR/"
install -m 644 "$ROOT/third_party/wayland/src/wayland-server.h" "$INCLUDE_DIR/"
install -m 644 "$ROOT/third_party/wayland/src/wayland-client-core.h" "$INCLUDE_DIR/"
install -m 644 "$ROOT/third_party/wayland/src/wayland-client.h" "$INCLUDE_DIR/"
install -m 644 "$ROOT/third_party/wayland/src/wayland-util.h" "$INCLUDE_DIR/"
install -m 644 "$BUILD/wayland-server-protocol.h" "$INCLUDE_DIR/"
install -m 644 "$BUILD/wayland-server-protocol-core.h" "$INCLUDE_DIR/"
install -m 644 "$BUILD/wayland-client-protocol.h" "$INCLUDE_DIR/"
install -m 644 "$BUILD/wayland-client-protocol-core.h" "$INCLUDE_DIR/"
install -m 644 "$BUILD/wayland-version.h" "$INCLUDE_DIR/"
install -m 644 "$ROOT/third_party/libinput/src/libinput.h" "$INCLUDE_DIR/"
install -m 644 "$ROOT/user/lib/udev-shim/libudev.h" "$INCLUDE_DIR/"
install -m 644 "$ROOT/third_party/seatd/include/libseat.h" "$INCLUDE_DIR/"
install -d "$INCLUDE_DIR/xkbcommon" "$INCLUDE_DIR/libdisplay-info" \
	"$INCLUDE_DIR/pixman-1"
install -m 644 "$ROOT/third_party/libxkbcommon/include/xkbcommon/"*.h \
	"$INCLUDE_DIR/xkbcommon/"
install -m 644 "$ROOT/third_party/libdisplay-info/include/libdisplay-info/"*.h \
	"$INCLUDE_DIR/libdisplay-info/"
install -m 644 "$ROOT/third_party/pixman/pixman/pixman.h" \
	"$INCLUDE_DIR/pixman-1/"
install -m 644 "$BUILD/wlroots/pixman/pixman/pixman-version.h" \
	"$INCLUDE_DIR/pixman-1/"
install -d "$INCLUDE_DIR/linux"
install -m 644 "$ROOT/include/uapi/compat/linux/input-event-codes.h" \
	"$INCLUDE_DIR/linux/"
install -d "$INCLUDE_DIR/linux/linux"
install -m 644 "$ROOT/third_party/libinput/include/linux/linux/input-event-codes.h" \
	"$INCLUDE_DIR/linux/linux/"

install -d "$INCLUDE_DIR/EGL" "$INCLUDE_DIR/GLES2" "$INCLUDE_DIR/KHR"
install -m 644 "$ROOT/third_party/mesa/include/EGL/"*.h "$INCLUDE_DIR/EGL/"
install -m 644 "$ROOT/third_party/mesa/include/GLES2/"*.h "$INCLUDE_DIR/GLES2/"
install -m 644 "$ROOT/third_party/mesa/include/KHR/"*.h "$INCLUDE_DIR/KHR/"
install -m 644 "$ROOT/third_party/mesa/src/gbm/main/gbm.h" "$INCLUDE_DIR/"

for library in wayland-server wayland-client input udev EGL GLESv2 gbm; do
	if [[ ! -f "$BUILD/lib${library}.so" ]]; then
		echo "ERROR: missing $BUILD/lib${library}.so" >&2
		exit 1
	fi
	install -m 755 "$BUILD/lib${library}.so" "$LIB_DIR/"
done

if [[ -f "$BUILD/wlroots/seatd/libseat.so.1" ]]; then
	install -m 755 "$BUILD/wlroots/seatd/libseat.so.1" "$LIB_DIR/libseat.so.1"
	install -m 755 "$BUILD/wlroots/seatd/libseat.so.1" "$LIB_DIR/libseat.so"
fi
if [[ -f "$BUILD/wlroots/libdisplay-info/libdisplay-info.so.0.4.0" ]]; then
	install -m 755 "$BUILD/wlroots/libdisplay-info/libdisplay-info.so.0.4.0" \
		"$LIB_DIR/libdisplay-info.so.0.4.0"
	install -m 755 "$BUILD/wlroots/libdisplay-info/libdisplay-info.so.0.4.0" \
		"$LIB_DIR/libdisplay-info.so.0"
	install -m 755 "$BUILD/wlroots/libdisplay-info/libdisplay-info.so.0.4.0" \
		"$LIB_DIR/libdisplay-info.so"
fi

cat > "$PC_DIR/wayland-server.pc" <<EOF
prefix=$SYSROOT/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: Wayland Server
Description: Wayland server library for the target OS
Version: 1.25.91
Libs: -L\${libdir} -lwayland-server
Cflags: -I\${includedir}
EOF

cat > "$PC_DIR/wayland-client.pc" <<EOF
prefix=$SYSROOT/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: Wayland Client
Description: Wayland client library for the target OS
Version: 1.25.91
Libs: -L\${libdir} -lwayland-client
Cflags: -I\${includedir}
EOF

cat > "$PC_DIR/libudev.pc" <<EOF
prefix=$SYSROOT/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libudev
Description: Target OS libudev compatibility library
Version: 255
Libs: -L\${libdir} -ludev
Cflags: -I\${includedir}
EOF

cat > "$PC_DIR/libinput.pc" <<EOF
prefix=$SYSROOT/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libinput
Description: Target OS libinput port
Version: 1.30.4
Requires.private: libudev
Libs: -L\${libdir} -linput
Cflags: -I\${includedir}
EOF

for module in egl glesv2 gbm; do
	case "$module" in
		egl) library=EGL ;;
		glesv2) library=GLESv2 ;;
		gbm) library=gbm ;;
	esac
	cat > "$PC_DIR/$module.pc" <<EOF
prefix=$SYSROOT/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: $module
Description: Mesa $module library for the target OS
Version: 26.1.4
Libs: -L\${libdir} -l$library
Cflags: -I\${includedir}
EOF
done

cat > "$PC_DIR/wayland-protocols.pc" <<EOF
prefix=/usr
datarootdir=\${prefix}/share
pkgdatadir=\${pc_sysrootdir}\${datarootdir}/wayland-protocols

Name: Wayland Protocols
Description: Wayland protocol XML files
Version: 1.49
EOF

cat > "$NATIVE_PC_DIR/wayland-scanner.pc" <<EOF
prefix=$BUILD
bindir=\${prefix}
wayland_scanner=\${bindir}/wayland-scanner

Name: Wayland Scanner
Description: Native Wayland protocol scanner
Version: 1.25.91
EOF

cat > "$NATIVE_PC_DIR/hwdata.pc" <<EOF
prefix=/usr
pkgdatadir=/usr/share/hwdata

Name: hwdata
Description: Native PCI and PNP identifier database
Version: 0
EOF

cat > "$NATIVE_FILE" <<EOF
[built-in options]
pkg_config_path = ['$NATIVE_PC_DIR']
EOF

echo "Prepared wlroots sysroot metadata in $PC_DIR"
