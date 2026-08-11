#!/bin/bash
# build.sh - CMake builds kernel + EFI bootloader + userspace ELF + generates image
set -e

# Configure git hooks path so pre-push check works out of the box
git config core.hooksPath build_script/githooks

# Build type: default Release, -d for Debug (with -g debug info)
BUILD_TYPE=Release
CMAKE_EXTRA=""
# Compiler: default clang, --gcc to switch. Both toolchains are supported.
OS_COMPILER=clang
RA_MAX_PAGES=16
PERF_TEST_NAME=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d)
            BUILD_TYPE=Debug
            shift
            ;;
        --test)
            CMAKE_EXTRA="$CMAKE_EXTRA -DTEST=1"
            BUILD_TEST=1
            shift
            ;;
        --sanitizer)
            CMAKE_EXTRA="$CMAKE_EXTRA -DSANITIZE=1"
            shift
            ;;
        --perf)
            PERF_TEST_NAME=""
            CMAKE_EXTRA="$CMAKE_EXTRA -DPERF=1 -DTEST=1"
            BUILD_PERF=1
            BUILD_TEST=1
            shift
            ;;
        --perf=*)
            PERF_TEST_NAME="${1#--perf=}"
            if [ -z "$PERF_TEST_NAME" ]; then
                echo "ERROR: --perf= requires a test name" >&2
                exit 1
            fi
            if [[ ! "$PERF_TEST_NAME" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*$ ]]; then
                echo "ERROR: invalid PERF test name: $PERF_TEST_NAME" >&2
                exit 1
            fi
            CMAKE_EXTRA="$CMAKE_EXTRA -DPERF=1 -DTEST=1"
            BUILD_PERF=1
            BUILD_TEST=1
            shift
            ;;
        --gcc)
            OS_COMPILER=gcc
            shift
            ;;
        --clang)
            OS_COMPILER=clang
            shift
            ;;
        --ra-pages)
            if [[ $# -lt 2 ]]; then
                echo "ERROR: --ra-pages requires off, 4, 8, or 16" >&2
                exit 1
            fi
            case "$2" in
                off|0) RA_MAX_PAGES=0 ;;
                4|8|16) RA_MAX_PAGES="$2" ;;
                *)
                    echo "ERROR: --ra-pages requires off, 4, 8, or 16" >&2
                    exit 1
                    ;;
            esac
            shift 2
            ;;
        *)
            echo "Usage: $0 [-d] [--test] [--sanitizer] [--perf[=test_name]] [--ra-pages off|4|8|16] [--gcc] [--clang]"
            exit 1
            ;;
    esac
done

# Ensure SANITIZE is explicitly set so CMake cache doesn't retain stale values
if ! echo "$CMAKE_EXTRA" | grep -q "SANITIZE="; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DSANITIZE=0"
fi
if [ "${BUILD_TEST:-0}" != "1" ]; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DTEST=0"
fi
if ! echo "$CMAKE_EXTRA" | grep -q "PERF="; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DPERF=0"
fi
# Always set the selector so a cached --perf=<name> does not affect --perf.
CMAKE_EXTRA="$CMAKE_EXTRA -DPERF_TEST_NAME=$PERF_TEST_NAME"

MESA_GALLIUM_DRIVERS=virgl,llvmpipe

# 1. CMake build (kernel + userspace)
mkdir -p build && cd build
cmake -GNinja \
      -DCMAKE_TOOLCHAIN_FILE=../build_script/cmake/toolchain-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
      -DOS_COMPILER=$OS_COMPILER \
      -DXOS_RA_MAX_PAGES=$RA_MAX_PAGES \
      $CMAKE_EXTRA \
      ..
ninja
cd ..

# install-headers.sh reads CC to locate the matching freestanding include dir
# (gcc → …/gcc/<v>/include, clang → …/clang/<v>/include).
if [[ "$OS_COMPILER" == "gcc" ]]; then
    export CC=gcc
else
    export CC=clang
fi

# 2. Publish sysroot artifacts (UAPI headers + libs → self-contained cross-target)
bash build_script/install-headers.sh
bash build_script/install-libs.sh

# 2b. Build libc++ into the sysroot. Must run AFTER install-headers/install-libs
# (sysroot ready: crt + stub + headers + libc.so exports) and BEFORE mkdisk (which
# ships libc++ into the image).
echo "=== Building libc++ ==="
bash build_script/build_libcxx.sh

# LLVM must exist before Mesa configures llvmpipe/lavapipe. The build is
# incremental and publishes target headers plus the monolithic target DSO.
echo "=== Building target LLVM/Clang toolchain ==="
bash build_script/build_llvm.sh

if [ "${BUILD_TEST:-0}" = "1" ]; then
    echo "=== Building libc++ smoke ELF ==="
    ninja -C build libcxx_smoke_elf
fi

# 3. Build the compositor dependency stack included by every image.
    echo "=== Building core seatd/libseat dependency ==="

    MESON_VENV=build/.mesa-venv
    if [[ ! -x "$MESON_VENV/bin/meson" ]]; then
        python3 -m venv "$MESON_VENV"
        "$MESON_VENV/bin/pip" install -q --upgrade pip
        "$MESON_VENV/bin/pip" install -q "meson>=1.4"
    fi
    export PATH="$MESON_VENV/bin:$PATH"

    WLROOTS_CROSS=build/wlroots-cross.txt
    CC_BIN=clang; CXX_BIN=clang++
    if [[ "$OS_COMPILER" == "gcc" ]]; then
        CC_BIN=gcc; CXX_BIN=g++
    fi
    SYSROOT="$(cd build/sysroot && pwd)"
    PYTHON_BIN="$(cd "$MESON_VENV" && pwd)/bin/python3"
    # wlroots controls its public ABI with wlroots.syms.  Unlike Mesa and the
    # local libraries, its headers do not mark each public function with a
    # visibility attribute, so a hidden default would produce an empty dynsym.
    LLVM_CONFIG="$(cd build_script && pwd)/llvm-config-target.py"
    sed -e "s#@CC@#$CC_BIN#g" -e "s#@CXX@#$CXX_BIN#g" -e "s#@PYTHON@#$PYTHON_BIN#g" \
        -e "s#@LLVM_CONFIG@#$LLVM_CONFIG#g" \
        -e "s#@SYSROOT@#$SYSROOT#g" \
        -e "s/-fvisibility=hidden/-fvisibility=default/g" \
        build_script/third_party/mesa/meson-cross-x86_64.txt.in > "$WLROOTS_CROSS"

    # All four projects are C-only. --prefix/--libdir make `meson install`
    # place their headers, libraries and pkg-config files directly in the target sysroot.
    build_wlroots_dependency() {
        local name="$1"
        local source="$2"
        shift 2
        local build_dir="build/wlroots/$name"
        # Meson's setup synopsis is `setup [options] <builddir> [sourcedir]`:
        # the build dir must follow all options, or meson treats the first
        # positional as the source dir. Put options first, then builddir+source.
        local setup=(--prefix /usr --libdir lib --buildtype=release -Doptimization=2 \
            --default-library=shared -Dwerror=false --cross-file "$WLROOTS_CROSS" \
            "$build_dir" "$source" "$@")

        if [[ -d "$build_dir" ]]; then
            meson setup --reconfigure "${setup[@]}"
        else
            meson setup "${setup[@]}"
        fi
        # Meson's default target (including `meson install`) also builds upstream
        # tests and benchmarks. Build only products Meson marks for installation,
        # then install without invoking the default target a second time.
        local installed_targets=()
        mapfile -t installed_targets < <(python3 - "$build_dir" "$name" <<'PY'
import json
import os
import sys

build_dir = sys.argv[1]
project = sys.argv[2]
targets = json.load(open(os.path.join(build_dir, 'meson-info', 'intro-targets.json')))
for target in targets:
    # This milestone ships libraries only, except for the seatd daemon. Some
    # projects mark diagnostic tools as installable; do not build those yet.
    is_library = target.get('type') == 'shared library'
    is_seatd_daemon = project == 'seatd' and target.get('name') == 'seatd'
    if not target.get('installed') or not (is_library or is_seatd_daemon):
        continue
    filenames = target.get('filename', [])
    if isinstance(filenames, str):
        filenames = [filenames]
    for filename in filenames:
        print(os.path.relpath(filename, build_dir))
PY
)
        if [[ ${#installed_targets[@]} -eq 0 ]]; then
            echo "ERROR: Meson reported no installable targets for $name" >&2
            return 1
        fi
        ninja -C "$build_dir" "${installed_targets[@]}"

        # libdisplay-info and seatd install diagnostic/helper executables in
        # addition to the products needed for wlroots. Install their selected
        # library/daemon payload explicitly so those unrelated programs stay
        # outside this first dependency milestone.
        case "$name" in
            libdisplay-info)
                install -d "$SYSROOT/usr/lib/pkgconfig" "$SYSROOT/usr/include"
                install -m 755 "$build_dir/libdisplay-info.so.0.4.0" "$SYSROOT/usr/lib/"
                # Upstream meson.build uses soversion=version_minor (4), so the
                # real SONAME baked into the ELF is libdisplay-info.so.4 — NOT
                # .so.0. wlroots links against the SONAME, so the runtime loader
                # must find libdisplay-info.so.4 at runtime.
                ln -sf libdisplay-info.so.0.4.0 "$SYSROOT/usr/lib/libdisplay-info.so.4"
                ln -sf libdisplay-info.so.4 "$SYSROOT/usr/lib/libdisplay-info.so"
                cp -a "$source/include/libdisplay-info" "$SYSROOT/usr/include/"
                install -m 644 "$build_dir/meson-private/libdisplay-info.pc" "$SYSROOT/usr/lib/pkgconfig/"
                ;;
            seatd)
                install -d "$SYSROOT/usr/lib/pkgconfig" "$SYSROOT/usr/include" "$SYSROOT/usr/bin"
                install -m 755 "$build_dir/libseat.so.1" "$SYSROOT/usr/lib/"
                ln -sf libseat.so.1 "$SYSROOT/usr/lib/libseat.so"
                # seatd is the first dynamic ELF in the boot sequence. Loading
                # the full libc.so from the cold FAT32 cache exceeds init's 2s
                # readiness deadline, so relink this small core daemon against
                # libc.a while keeping libseat shared for desktop clients.
                "$CC_BIN" -m64 -static --sysroot="$SYSROOT" -nostdlib -nodefaultlibs \
                    -Wl,--hash-style=gnu -o "$build_dir/seatd.static" \
                    "$SYSROOT/usr/lib/crt1.o" "$SYSROOT/usr/lib/crti.o" \
                    "$build_dir"/seatd.p/*.o -Wl,--start-group \
                    "$SYSROOT/usr/lib/libc.a" -Wl,--end-group \
                    "$SYSROOT/usr/lib/crtn.o"
                if readelf -l "$build_dir/seatd.static" | grep -q 'interpreter'; then
                    echo "ERROR: static seatd unexpectedly has a program interpreter" >&2
                    return 1
                fi
                install -m 755 "$build_dir/seatd.static" "$SYSROOT/usr/bin/seatd"
                install -m 644 "$source/include/libseat.h" "$SYSROOT/usr/include/"
                install -m 644 "$build_dir/meson-private/libseat.pc" "$SYSROOT/usr/lib/pkgconfig/"
                ;;
            *)
                meson install -C "$build_dir" --no-rebuild --destdir "$SYSROOT"
                ;;
        esac
    }

    echo "=== Building wlroots prerequisites (pixman, xkbcommon, libdisplay-info) ==="
    build_wlroots_dependency pixman third_party/pixman \
        -Dgtk=disabled -Ddemos=disabled -Dtests=disabled -Dlibpng=disabled -Dopenmp=disabled
    build_wlroots_dependency libxkbcommon third_party/libxkbcommon \
        -Denable-tools=false -Denable-x11=false -Denable-xkbregistry=false \
        -Denable-wayland=false -Denable-bash-completion=false
    # libdisplay-info needs default visibility so its version script can
    # export the public di_* ABI.
    build_wlroots_dependency libdisplay-info third_party/libdisplay-info \
        -Dc_args=-fvisibility=default
    build_wlroots_dependency seatd third_party/seatd \
        -Dlibseat-logind=disabled -Dlibseat-builtin=disabled -Dlibseat-seatd=enabled \
        -Dserver=enabled -Dexamples=disabled -Dman-pages=disabled \
        -Ddefaultpath=/run/seatd.sock

    # Build the target-side WF-6 diagnostic against the just-staged libseat.
    "$CC_BIN" -m64 -O2 -fPIC --sysroot="$SYSROOT" -nodefaultlibs \
        -I"$SYSROOT/usr/include" -c user/seat/seat_session_smoke.c \
        -o build/seat-session-smoke.o
    "$CC_BIN" -m64 -fno-pie -no-pie --sysroot="$SYSROOT" \
        -nostdlib -nodefaultlibs -Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1 \
        -Wl,--hash-style=gnu -Wl,--no-as-needed -Wl,--allow-shlib-undefined \
        -o build/seat-session-smoke \
        "$SYSROOT/usr/lib/Scrt1.o" "$SYSROOT/usr/lib/crti.o" \
        build/seat-session-smoke.o -L"$SYSROOT/usr/lib" -lseat -lc \
        "$SYSROOT/usr/lib/crtn.o"
    "$CC_BIN" -m64 -O2 -fPIC --sysroot="$SYSROOT" -nodefaultlibs \
        -I"$SYSROOT/usr/include" -c user/seat/seat_protocol_negative.c \
        -o build/seat-protocol-negative.o
    "$CC_BIN" -m64 -fno-pie -no-pie --sysroot="$SYSROOT" \
        -nostdlib -nodefaultlibs -Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1 \
        -Wl,--hash-style=gnu -Wl,--no-as-needed -Wl,--allow-shlib-undefined \
        -o build/seat-protocol-negative \
        "$SYSROOT/usr/lib/Scrt1.o" "$SYSROOT/usr/lib/crti.o" \
        build/seat-protocol-negative.o -L"$SYSROOT/usr/lib" -lc \
        "$SYSROOT/usr/lib/crtn.o"

    if readelf -d "$SYSROOT/usr/lib/libseat.so.1" "$SYSROOT/usr/bin/seatd" | \
       grep -Eq 'lib(systemd|elogind|dbus)'; then
        echo "ERROR: seatd/libseat pulled in a forbidden session dependency" >&2
        exit 1
    fi

    # Disk images are FAT32, so copy each runtime name as a real file instead
    # of preserving the symlinks Meson installs into the sysroot.
    python3 - "$SYSROOT" <<'PY'
import os
import shutil
import sys

sysroot = sys.argv[1]
libdir = os.path.join(sysroot, 'usr', 'lib')
staged = {
    'libseat.so': ['libseat.so', 'libseat.so.1'],
    'libpixman-1.so': ['libpixman-1.so', 'libpixman-1.so.0', 'libpixman-1.so.0.46.4'],
    'libxkbcommon.so': ['libxkbcommon.so', 'libxkbcommon.so.0', 'libxkbcommon.so.0.13.2'],
    'libdisplay-info.so': ['libdisplay-info.so', 'libdisplay-info.so.4', 'libdisplay-info.so.0.4.0'],
}
for names in staged.values():
    source = next((os.path.join(libdir, name) for name in reversed(names)
                   if os.path.exists(os.path.join(libdir, name))), None)
    if source is None:
        raise SystemExit(f'ERROR: missing installed wlroots prerequisite: {names[0]}')
    for name in names:
        shutil.copy2(source, os.path.join('build', name), follow_symlinks=True)

seatd = os.path.join(sysroot, 'usr', 'bin', 'seatd')
if not os.path.isfile(seatd):
    raise SystemExit('ERROR: missing installed seatd executable')
shutil.copy2(seatd, os.path.join('build', 'seatd'))
PY

    # These TEST-image ELFs link the just-staged external libraries, so build
    # them after sysroot preparation rather than in the initial CMake pass.
    if [ "${BUILD_TEST:-0}" = "1" ]; then
        echo "=== Building TEST dependency smoke ELFs ==="
        ninja -C build \
            test_pixman_smoke_dyn_elf \
            test_display_info_smoke_dyn_elf \
            test_xkbcommon_smoke_dyn_elf
    fi

# 3. Mesa cross-build (Meson/Ninja handles incremental rebuilds).
#    Softpipe first to validate the cross pipeline; switch to virgl by exporting
#    MESA_DRIVER=virgl (one-line option change, identical codegen — see step2.md).
#    Runs after the sysroot is populated (deps 2) and before mkdisk (so .so land in image).
#    The C++ GLSL compiler consumes the libc++ sysroot built in the preceding step.
if [[ ! -f build/sysroot/usr/lib/libc++.so ]]; then
    echo "Mesa cross-build needs the default libc++ sysroot." >&2
    exit 1
fi
MESADIR=build/mesa
CROSS=build/mesa-cross.txt
NATIVE=build/mesa-native.txt
NATIVE_PCDIR=build/mesa-native-pkgconfig
CC_BIN=clang; CXX_BIN=clang++
if [[ "$OS_COMPILER" == "gcc" ]]; then
    CC_BIN=gcc; CXX_BIN=g++
fi

# Mesa pins `meson_version : '>= 1.4.0'` but the host apt meson is 1.3.2. Build an
# isolated venv with a recent meson + mako (Mesa's python codegen — nir/glsl/glapi —
# needs mako). Idempotent: created only when build/.mesa-venv/bin/meson is missing;
# cached wheels make recreation fast even after `rm -rf build`. Prepending the venv
# bin to PATH lets `meson setup`/`ninja` pick up the venv meson + host ninja. The
# venv's python3.12 is also the [binaries] python in the cross-file (absolute path,
# so meson's find_program version probe resolves regardless of cwd — see cross-file
# header), and it carries the modules Mesa's configure-time run_command imports:
#   mako (>=0.8, nir/glsl/glapi codegen), packaging (mako version-compare; distutils
#   is gone in py3.12 so packaging is mandatory), pyyaml (u_format.yaml + driconf).
MESA_VENV=build/.mesa-venv
# Create the venv if missing, then verify every required module is importable
# every run (not just when meson is absent). A half-populated venv — e.g. meson
# present but mako/packaging/pyyaml missing after a pip install hiccup — would
# otherwise slip through the old `[[ ! -x .../meson ]]` gate and crash Mesa's
# configure-time mako probe. Reinstalling the wheels is cheap (cached), so we
# always re-check rather than trust a stale venv.
if [[ ! -x "$MESA_VENV/bin/python3" ]]; then
    python3 -m venv "$MESA_VENV"
    "$MESA_VENV/bin/pip" install -q --upgrade pip
fi
# -q on the import probe keeps this quiet when all modules are present.
# (meson ships as a bin/meson script, not an importable module — probe it via -x.)
if [[ ! -x "$MESA_VENV/bin/meson" ]] || \
   ! "$MESA_VENV/bin/python3" -c "import mako, packaging, yaml" 2>/dev/null; then
    "$MESA_VENV/bin/pip" install -q "meson>=1.4" mako packaging pyyaml
fi
export PATH="$MESA_VENV/bin:$PATH"
PYTHON_BIN="$(cd "$MESA_VENV" && pwd)/bin/python3"
LLVM_CONFIG="$(cd build_script && pwd)/llvm-config-target.py"

# Generate the cross-file (CC/CXX/PYTHON/SYSROOT absolute). venv must exist first (above).
# SYSROOT is the same build/sysroot install-headers/ls populates; the cross-file uses it
# for the compiler/linker --sysroot flag AND pkg_config_libdir/sys_root.
SYSROOT="$(cd build/sysroot && pwd)"
sed -e "s#@CC@#$CC_BIN#g" -e "s#@CXX@#$CXX_BIN#g" -e "s#@PYTHON@#$PYTHON_BIN#g" \
    -e "s#@LLVM_CONFIG@#$LLVM_CONFIG#g" \
    -e "s#@SYSROOT@#$SYSROOT#g" \
    build_script/third_party/mesa/meson-cross-x86_64.txt.in > "$CROSS"

# wayland-scanner runs on the build machine, so it does not belong in the cross
# file. Pin the scanner built from our Wayland submodule instead of accepting an
# older host installation (Mesa requires it to match wayland-client's version).
WAYLAND_SCANNER="$(cd build && pwd)/wayland-scanner"
if [[ ! -x "$WAYLAND_SCANNER" ]]; then
    echo "Mesa cross-build needs $WAYLAND_SCANNER." >&2
    exit 1
fi
mkdir -p "$NATIVE_PCDIR"
cat > "$NATIVE_PCDIR/wayland-scanner.pc" <<EOF
prefix=$(cd build && pwd)
bindir=\${prefix}
wayland_scanner=\${bindir}/wayland-scanner

Name: Wayland Scanner
Description: Wayland protocol scanner built from the repository submodule
Version: 1.25.91
EOF
NATIVE_PCDIR_ABS="$(cd "$NATIVE_PCDIR" && pwd)"
cat > "$NATIVE" <<EOF
[binaries]
wayland-scanner = '$WAYLAND_SCANNER'

[built-in options]
pkg_config_path = ['$NATIVE_PCDIR_ABS']
EOF

# Emit libdrm/zlib/expat/ffi .pc into the sysroot so meson dependency() resolves.
bash build_script/gen-pkgconfig.sh

if [[ ! -f "$SYSROOT/usr/lib/libLLVM.so.18.1" || ! -d "$SYSROOT/usr/include/llvm" ]]; then
    echo "ERROR: Mesa Vulkan build requires target LLVM headers and libLLVM.so.18.1" >&2
    exit 1
fi
# Mesa's Vulkan display WSI source is compiled on Linux even though the stage-1
# smoke creates no surface. Publish the project's existing target libudev API.
install -m 644 user/lib/udev-shim/libudev.h "$SYSROOT/usr/include/libudev.h"

echo "=== Building Vulkan Headers/Loader ==="
bash build_script/build_vulkan.sh

GALLIUM="${MESA_GALLIUM_DRIVERS:-virgl,llvmpipe}"
MESA_SETUP=(
    "$MESADIR" third_party/mesa
    --buildtype release -Doptimization=2
    # Meson persists command-line built-in options across --wipe. Spell these
    # out so an older build configured with a partial c_args/cpp_args override
    # cannot silently mask the complete values in the cross file.
    "-Dc_args=-m64 -fPIC --sysroot=$SYSROOT -nodefaultlibs -fvisibility=hidden -Wno-macro-redefined"
    "-Dcpp_args=-m64 -fPIC --sysroot=$SYSROOT -nodefaultlibs -stdlib=libc++ -nostdinc++ -I$SYSROOT/usr/include/c++/v1 -fvisibility=hidden -Wno-macro-redefined"
    -Dcpp_rtti=false
    "-Dgallium-drivers=$GALLIUM"
    -Dglx=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
    -Degl=enabled -Dgbm=enabled
    -Dplatforms=wayland -Dvulkan-drivers=swrast -Dllvm=enabled
    -Dshared-llvm=enabled -Ddraw-use-llvm=true
    -Dgallium-va=disabled -Dgallium-rusticl=false -Dvideo-codecs=
    -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled
    -Dspirv-tools=disabled
    --cross-file "$CROSS"
    --native-file "$NATIVE"
)
if [[ -d "$MESADIR" ]]; then
    # Machine-file changes affect Meson's configure probes, but a normal
    # reconfigure retains failed probes. Wipe when either machine file changes.
    CROSS_STAMP="$MESADIR/.xos-cross-file.sha256"
    CROSS_SUM="$({ sha256sum "$CROSS" "$NATIVE" include/uapi/linux/udmabuf.h \
        include/uapi/linux/dma-buf.h; printf '%s\n' "${MESA_SETUP[*]}"; } | sha256sum | awk '{print $1}')"
    if [[ ! -f "$CROSS_STAMP" || "$(<"$CROSS_STAMP")" != "$CROSS_SUM" ]]; then
        meson setup --wipe "${MESA_SETUP[@]}"
        printf '%s\n' "$CROSS_SUM" > "$CROSS_STAMP"
    else
        meson setup --reconfigure "${MESA_SETUP[@]}"
    fi
else
    meson setup "${MESA_SETUP[@]}"
    { sha256sum "$CROSS" "$NATIVE"; printf '%s\n' "${MESA_SETUP[*]}"; } | sha256sum | awk '{print $1}' > "$MESADIR/.xos-cross-file.sha256"
fi
ninja -C "$MESADIR"

# Stage the Mesa EGL, GBM, Gallium and lavapipe products used at runtime. Meson
# introspection supplies the real output path for each target.
python3 - "$MESADIR" <<'PY'
import json, os, shutil, sys
mesadir = sys.argv[1]
# EGL and GBM are versioned; Gallium, dri_gbm and lavapipe have one runtime name.
libs = {
    "libEGL.so":    ["libEGL.so",    "libEGL.so.1",    "libEGL.so.1.0.0"],
    "libgbm.so":    ["libgbm.so",    "libgbm.so.1",    "libgbm.so.1.0.0"],
    "libgallium-26.1.4.so": ["libgallium-26.1.4.so"],
    "dri_gbm.so":           ["dri_gbm.so"],
    "libvulkan_lvp.so":     ["libvulkan_lvp.so"],
}
# A reconfigure doesn't clean GLESv2 products staged by an older build.
for name in ("libGLESv2.so", "libGLESv2.so.2", "libGLESv2.so.2.0.0"):
    path = os.path.join("build", name)
    if os.path.exists(path):
        os.remove(path)
# Map each wanted real file to the Meson target filename that produces it.
targets = json.load(open(os.path.join(mesadir, "meson-info", "intro-targets.json")))
real_files = {}  # real basename -> source path
for t in targets:
    # Meson 1.4+ reports target filenames as a list, while older releases used
    # a single string.  A shared library can also legitimately have more than
    # one output, so record every reported path.
    outputs = t.get("filename", [])
    if isinstance(outputs, str):
        outputs = [outputs]
    for out in outputs:
        if out:
            real_files[os.path.basename(out)] = out

staged = set()
# For each versioned lib, the real file is the longest .so.<ver> name meson actually
# built; the others (soname, linker name) are aliases → copy the real file to every
# name in the group (real copies, not symlinks — FAT32-safe). Unversioned libs copy
# their single name.
for group in libs.values():
    # real = the longest .so.<ver> name meson actually built; others are aliases of it.
    real = max(group, key=len)
    src = real_files.get(real)
    if src is None:
        # fall back: try any name in the group meson lists (unversioned case)
        src = next((real_files[n] for n in group if n in real_files), None)
    if src is None:
        print(f"WARNING: Mesa product not found for {group[0]} (looked for {real})", file=sys.stderr)
        continue
    for name in group:
        dst = os.path.join("build", name)
        shutil.copy2(src, dst)
        staged.add(name)
print(f"  mesa stage: {len(staged)} files -> build/ ({sorted(staged)})")
missing = [g[0] for g in libs.values() if not any(n in staged for n in g)]
if missing:
    raise SystemExit(f"ERROR: missing Mesa products: {missing}")

manifest_src = real_files.get("lvp_icd.x86_64.json")
if manifest_src is None:
    raise SystemExit("ERROR: Mesa did not produce lvp_icd.x86_64.json")
with open(manifest_src, encoding="utf-8") as src:
    manifest = json.load(src)
library = manifest.get("ICD", {}).get("library_path")
if not isinstance(library, str):
    raise SystemExit("ERROR: lvp ICD manifest has no string ICD.library_path")
manifest["ICD"]["library_path"] = "/lib/libvulkan_lvp.so"
manifest_dst = os.path.join("build", "lvp_icd.x86_64.json")
with open(manifest_dst, "w", encoding="utf-8") as dst:
    json.dump(manifest, dst, indent=2)
    dst.write("\n")
os.makedirs(os.path.join("build", "sysroot", "usr", "share", "vulkan", "icd.d"), exist_ok=True)
shutil.copy2(manifest_dst, os.path.join("build", "sysroot", "usr", "share", "vulkan", "icd.d", "lvp_icd.x86_64.json"))
shutil.copy2(os.path.join("build", "libvulkan_lvp.so"), os.path.join("build", "sysroot", "usr", "lib", "libvulkan_lvp.so"))
PY

python3 - "$MESADIR/meson-info/intro-buildoptions.json" <<'PY'
import json, sys
options = {entry["name"]: entry["value"] for entry in json.load(open(sys.argv[1]))}
expected = {
    "vulkan-drivers": ["swrast"],
    "gallium-drivers": ["virgl", "llvmpipe"],
    "llvm": "enabled",
    "shared-llvm": "enabled",
    "platforms": ["wayland"],
    "egl": "enabled",
    "gles2": "disabled",
}
for name, wanted in expected.items():
    if options.get(name) != wanted:
        raise SystemExit(f"ERROR: Mesa option {name}={options.get(name)!r}, expected {wanted!r}")
print("Mesa Vulkan feature audit: PASS")
PY

# Build the compositor stack after Mesa and the target sysroot are ready.
    echo "=== Building wlroots 0.20.2 ==="
    bash build_script/third_party/wlroots/prepare-sysroot.sh
    SYSROOT="$(cd build/sysroot && pwd)"
    # EGL and GBM load the Mesa Gallium backend at runtime, so keep the
    # megadriver pair in the target sysroot used to link and audit wlroots.
    for gallium_so in libgallium-26.1.4.so dri_gbm.so; do
        if [[ ! -f "build/$gallium_so" ]]; then
            echo "ERROR: missing build/$gallium_so (run Mesa stage first)" >&2
            exit 1
        fi
        install -m 755 "build/$gallium_so" "$SYSROOT/usr/lib/"
    done
    WLROOTS_BUILD=build/wlroots/wlroots
    WLROOTS_NATIVE=build/wlroots-native.txt
    WLROOTS_SETUP=(
        --prefix /usr --libdir lib --buildtype release -Doptimization=2
        --default-library shared --wrap-mode nodownload
        --cross-file build/wlroots-cross.txt --native-file "$WLROOTS_NATIVE"
        -Dauto_features=disabled -Dbackends=drm,libinput -Drenderers=auto
        -Dallocators=gbm,udmabuf -Dsession=enabled -Dxwayland=disabled
        -Dxcb-errors=disabled -Dcolor-management=disabled
        # tinywl is maintained in user/compositor and linked against the
        # installed shared library; do not build wlroots' upstream examples.
        -Dlibliftoff=disabled -Dexamples=false -Dwerror=false
        "$WLROOTS_BUILD" third_party/wlroots
    )
    WLROOTS_CONFIG_SUM="$({ sha256sum build/wlroots-cross.txt "$WLROOTS_NATIVE" \
        include/uapi/linux/udmabuf.h include/uapi/linux/dma-buf.h \
        include/uapi/linux/version.h; \
        git -C third_party/wlroots rev-parse HEAD; printf '%s\n' "${WLROOTS_SETUP[*]}"; } | sha256sum | awk '{print $1}')"
    WLROOTS_STAMP="$WLROOTS_BUILD/.xos-config.sha256"
    if [ -d "$WLROOTS_BUILD" ] && \
       { [ ! -f "$WLROOTS_STAMP" ] || [ "$(<"$WLROOTS_STAMP")" != "$WLROOTS_CONFIG_SUM" ]; }; then
        meson setup --wipe "${WLROOTS_SETUP[@]}"
    elif [ -d "$WLROOTS_BUILD" ]; then
        meson setup --reconfigure "${WLROOTS_SETUP[@]}"
    else
        meson setup "${WLROOTS_SETUP[@]}"
    fi
    printf '%s\n' "$WLROOTS_CONFIG_SUM" > "$WLROOTS_STAMP"
    ninja -C "$WLROOTS_BUILD" libwlroots-0.20.so
    meson install -C "$WLROOTS_BUILD" --no-rebuild --destdir "$SYSROOT"

    MESON_SUMMARY="$WLROOTS_BUILD/meson-logs/meson-log.txt"
    for feature in 'drm-backend: YES' 'libinput-backend: YES' 'session: YES' \
                   'gles2-renderer: NO' \
                   'gbm-allocator: YES' 'udmabuf-allocator: YES' \
                   'x11-backend: NO' 'xwayland: NO' 'vulkan-renderer: NO' \
                   'color-management: NO' 'libliftoff: NO'; do
        feature_name="${feature%: *}"
        feature_value="${feature##*: }"
        if ! grep -Eq "^[[:space:]]*$feature_name[[:space:]]*:[[:space:]]*$feature_value$" \
             "$MESON_SUMMARY"; then
            echo "ERROR: wlroots feature matrix mismatch: $feature" >&2
            exit 1
        fi
    done

    python3 - "$WLROOTS_BUILD/meson-info/intro-buildoptions.json" <<'PY'
import json
import sys

options = {item["name"]: item["value"] for item in json.load(open(sys.argv[1]))}
expected = {
    "renderers": ["auto"],
    "backends": ["drm", "libinput"],
    "allocators": ["gbm", "udmabuf"],
}
for name, wanted in expected.items():
    if options.get(name) != wanted:
        raise SystemExit(
            f"ERROR: wlroots option {name}={options.get(name)!r}, expected {wanted!r}")
print("wlroots Meson option audit: PASS")
PY

    WLROOTS_LIB="$SYSROOT/usr/lib/libwlroots-0.20.so"
    install -m 755 "$WLROOTS_LIB" build/libwlroots-0.20.so

    # These targets link external products staged by Mesa/wlroots and therefore
    # are built after the corresponding Meson installs.
    echo "=== Building Wayland desktop executables ==="
    ninja -C build tinywl_dyn_elf terminal_dyn_elf
    if [ "$BUILD_TEST" = "1" ]; then
        ninja -C build test_terminal_sgr_dyn_elf
    fi

        # --- ELF static audit: build/tinywl.elf + build/libwlroots-0.20.so ---
        # Judge 1: interpreter (executable ELF must be musl; .so has no PT_INTERP, skip)
        tinywl_interp="$(readelf -lW build/tinywl.elf 2>/dev/null | \
            awk '/interpreter:/ {print $NF}' | tr -d '[]')"
        if [ "$tinywl_interp" != "/lib/ld-musl-x86_64.so.1" ]; then
            echo "ERROR: build/tinywl.elf interpreter '$tinywl_interp' != /lib/ld-musl-x86_64.so.1" >&2
            exit 1
        fi
        # Judge 2: forbidden dependency grep
        if LC_ALL=C readelf -dW build/tinywl.elf build/libwlroots-0.20.so | \
           grep -Eiq 'lib(X11|xcb|systemd|elogind|dbus|liftoff)|libstdc\+\+|llvmpipe'; then
            echo "ERROR: forbidden dependency in wlroots runtime" >&2
            exit 1
        fi
        # Judge 3: DT_NEEDED closure ⊆ sysroot/image (single-layer filename
        # existence; the sysroot library set is closed, so direct NEEDED fully
        # resolvable == transitive closure resolvable — no "passes layer 1 but a
        # deep dep is missing" gap).
        audit_allowed="$( ( ls "$SYSROOT/usr/lib"/*.so "$SYSROOT/usr/lib"/*.so.* 2>/dev/null; \
                           ls build/*.so build/*.so.* 2>/dev/null ) \
                         | xargs -n1 basename 2>/dev/null | sort -u )"
        # LC_ALL=C forces English readelf output so the awk field layout is stable
        # (a zh_CN.UTF-8 host renders the NEEDED value as "共享库：[libc.so]", a
        # single space-free field, which breaks the $NF extraction). The bracket
        # class [\[\]] strips the surrounding [] that readelf wraps the soname in.
        audit_needed="$(LC_ALL=C readelf -dW build/tinywl.elf build/libwlroots-0.20.so 2>/dev/null \
                        | awk '/NEEDED/ {gsub(/[\[\]]/,"",$NF); print $NF}' | sort -u)"
        audit_missing=""
        for n in $audit_needed; do
            echo "$audit_allowed" | grep -qx "$n" || audit_missing="$audit_missing $n"
        done
        if [ -n "$audit_missing" ]; then
            echo "ERROR: DT_NEEDED not resolvable in sysroot/image:$audit_missing" >&2
            exit 1
        fi
    echo "wlroots ELF audit: PASS (tinywl.elf + libwlroots-0.20.so)"

# Build API smoke targets after their Mesa libraries have been staged.
if echo "$CMAKE_EXTRA" | grep -q "TEST=1"; then
    echo "=== Building EGL and Vulkan smoke ELFs ==="
    ninja -C build test_egl_smoke_elf test_vulkan_smoke_elf
fi

if [ "${BUILD_TEST:-0}" = "1" ]; then
    echo "=== Auditing Vulkan runtime closure ==="
    python3 build_script/audit-vulkan.py
fi

# 4. Generate disk.img (single disk, two partitions: ESP + root FAT32)
TEST="${TEST:-0}"
if echo "$CMAKE_EXTRA" | grep -q "TEST=1"; then
    TEST=1
fi
export TEST

PERF="${BUILD_PERF:-0}"
export PERF

if [ "$PERF" = "1" ]; then
    bash build_script/perf-symbols.sh
fi

./build_script/mkdisk.sh
