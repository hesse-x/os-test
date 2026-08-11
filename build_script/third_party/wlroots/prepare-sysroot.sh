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
PROTOCOL_BUILD="$BUILD/wlroots/wayland-protocols"
install -d "$INCLUDE_DIR" "$LIB_DIR" "$PC_DIR" "$NATIVE_PC_DIR"

# wayland-protocols 1.49 generates its *-enum.h headers with
# `wayland-scanner --strict`, whose DTD validation fails on the host scanner
# (its built-in DTD predates the `frozen` interface attribute). Pin the scanner
# built from our Wayland submodule through a native file + pkg-config dir, the
# same pattern the Mesa stage uses in build.sh.
cat > "$NATIVE_PC_DIR/wayland-scanner.pc" <<EOF
prefix=$BUILD
bindir=\${prefix}
wayland_scanner=\${bindir}/wayland-scanner

Name: Wayland Scanner
Description: Native Wayland protocol scanner
Version: 1.25.91
EOF

cat > "$NATIVE_PC_DIR/hwdata.pc" <<EOF
prefix=$ROOT/build_script/third_party/hwdata
pkgdatadir=\${prefix}

Name: hwdata
Description: Native PCI and PNP identifier database
Version: 0
EOF

cat > "$NATIVE_FILE" <<EOF
[binaries]
wayland-scanner = '$BUILD/wayland-scanner'

[built-in options]
pkg_config_path = ['$NATIVE_PC_DIR']
EOF

# wayland-protocols is a data package, not a runtime library. Install its XML
# and pkg-config metadata with the upstream Meson rules so the sysroot layout
# and advertised version stay in sync with the vendored submodule.
protocol_setup=(--prefix /usr -Dtests=false --native-file "$NATIVE_FILE" \
	"$PROTOCOL_BUILD" "$ROOT/third_party/wayland-protocols")
if [[ -d "$PROTOCOL_BUILD" ]]; then
	# A plain reconfigure retains the previously resolved host-scanner probe;
	# wipe so the native file above re-pins the OS-built scanner.
	meson setup --wipe "${protocol_setup[@]}"
else
	meson setup "${protocol_setup[@]}"
fi
# The enum headers are Meson custom targets in the install plan, so build them
# (no --no-rebuild) with the pinned scanner.
meson install -C "$PROTOCOL_BUILD" --destdir "$SYSROOT"

install -d "$INCLUDE_DIR/wayland-protocols"
# wlroots public headers include the generated protocol enum headers. Generate
# the complete vendored set so consumers never depend on host protocol data.
while IFS= read -r xml; do
	name="$(basename "${xml%.xml}")-enum.h"
	"$BUILD/wayland-scanner" --strict enum-header "$xml" \
		"$INCLUDE_DIR/wayland-protocols/$name"
done < <(find "$ROOT/third_party/wayland-protocols" -name '*.xml' -type f | sort)

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
install -m 644 "$ROOT/include/uapi/linux/input-event-codes.h" \
	"$INCLUDE_DIR/linux/"

# Keep EGL for GBM/DRI integration, but remove GLESv2 products left by an
# older incremental build.
rm -rf "$INCLUDE_DIR/GLES2"
rm -f "$LIB_DIR/libGLESv2.so" \
	"$LIB_DIR/libGLESv2.so.2" "$LIB_DIR/libGLESv2.so.2.0.0" \
	"$PC_DIR/glesv2.pc"
install -d "$INCLUDE_DIR/EGL" "$INCLUDE_DIR/KHR"
install -m 644 "$ROOT/third_party/mesa/include/EGL/"*.h "$INCLUDE_DIR/EGL/"
install -m 644 "$ROOT/third_party/mesa/include/KHR/"*.h "$INCLUDE_DIR/KHR/"
install -m 644 "$ROOT/third_party/mesa/src/gbm/main/gbm.h" "$INCLUDE_DIR/"

for library in wayland-server wayland-client input udev EGL gbm; do
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

for module in egl gbm; do
	case "$module" in
		egl) library=EGL ;;
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

echo "Prepared wlroots sysroot metadata in $PC_DIR"
