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

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d)
            BUILD_TYPE=Debug
            shift
            ;;
        --test)
            CMAKE_EXTRA="$CMAKE_EXTRA -DTEST=1"
            # The test image exercises every optional runtime integration.
            FORCE_LIBCXX=1
            FORCE_MESA=1
            BUILD_WLROOTS_DEPS=1
            shift
            ;;
        --sanitizer)
            CMAKE_EXTRA="$CMAKE_EXTRA -DSANITIZE=1"
            shift
            ;;
        --perf)
            CMAKE_EXTRA="$CMAKE_EXTRA -DPERF=1"
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
        --cxx)
            # opt-in: build libc++ (LLVM runtimes) into build/sysroot after the
            # default flow, so mkdisk can ship it into disk.img. Default flow
            # does not build libc++ (zero cost). See refact_cmake.md.
            FORCE_LIBCXX=1
            shift
            ;;
        --mesa)
            # Build Mesa only when explicitly requested; it depends on the
            # libc++ sysroot supplied by --cxx.
            FORCE_MESA=1
            shift
            ;;
        --wlroots)
            # This first wlroots milestone only builds its four standalone
            # dependencies. wlroots itself is intentionally not built yet.
            BUILD_WLROOTS_DEPS=1
            shift
            ;;
        --desktop=compositor)
            CMAKE_EXTRA="$CMAKE_EXTRA -DDESKTOP_COMPOSITOR=1"
            BUILD_WLROOTS_DEPS=1
            shift
            ;;
        *)
            echo "Usage: $0 [-d] [--test] [--sanitizer] [--perf] [--gcc] [--clang] [--cxx] [--mesa] [--wlroots] [--desktop=compositor]"
            exit 1
            ;;
    esac
done

# Ensure SANITIZE is explicitly set so CMake cache doesn't retain stale values
if ! echo "$CMAKE_EXTRA" | grep -q "SANITIZE="; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DSANITIZE=0"
fi

# A normal build is incremental but self-healing: missing runtime products cause
# their producer to run. The explicit flags force their respective build even
# when the products are already complete.
libcxx_complete=1
for artifact in \
    build/sysroot/usr/lib/libc++.so \
    build/sysroot/usr/lib/libc++.so.1.0 \
    build/sysroot/usr/lib/libc++abi.so.1.0 \
    build/sysroot/usr/lib/libunwind.so.1.0 \
    build/sysroot/usr/include/c++/v1/vector; do
    [ -f "$artifact" ] || libcxx_complete=0
done
if [ "${FORCE_LIBCXX:-0}" = "1" ] || [ "$libcxx_complete" = "0" ]; then
    BUILD_LIBCXX=1
fi

# Check both Meson's products and the FAT32-safe build/ aliases used by mkdisk.
mesa_complete=1
for artifact in \
    build/mesa/src/egl/libEGL.so.1.0.0 \
    build/mesa/src/mesa/glapi/es2api/libGLESv2.so.2.0.0 \
    build/mesa/src/gbm/libgbm.so.1.0.0 \
    build/mesa/src/gallium/targets/dri/libgallium-26.1.4.so \
    build/mesa/src/gbm/backends/dri/dri_gbm.so \
    build/libEGL.so build/libGLESv2.so build/libgbm.so \
    build/libgallium-26.1.4.so build/dri_gbm.so; do
    [ -f "$artifact" ] || mesa_complete=0
done
if [ "${FORCE_MESA:-0}" = "1" ] || [ "$mesa_complete" = "0" ]; then
    BUILD_MESA=1
fi

# CMake needs the image entries even when the corresponding compiler step is
# skipped because complete artifacts are reused from a previous build.
CMAKE_EXTRA="$CMAKE_EXTRA -DLIBCXX=1 -DMESA=1"
if [ "${BUILD_WLROOTS_DEPS:-0}" = "1" ]; then
    CMAKE_EXTRA="$CMAKE_EXTRA -DWLROOTS_DEPS=1"
else
    CMAKE_EXTRA="$CMAKE_EXTRA -DWLROOTS_DEPS=0"
fi

# 1. CMake build (kernel + userspace)
mkdir -p build && cd build
cmake -GNinja \
      -DCMAKE_TOOLCHAIN_FILE=../build_script/cmake/toolchain-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
      -DOS_COMPILER=$OS_COMPILER \
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

# 2b. (opt-in) Build libc++ into the sysroot. Must run AFTER install-headers/install-libs
# (sysroot ready: crt + stub + headers + libc.so exports) and BEFORE mkdisk (which
# probe-ships libc++ into the image). Default flow skips this entirely.
if [ "${BUILD_LIBCXX:-0}" = "1" ]; then
    echo "=== Building libc++ (opt-in, --cxx) ==="
    bash build_script/build_libcxx.sh

    # The libc++ smoke ELF (user/test/libcxx_smoke.cpp, -DLIBCXX=1) is EXCLUDE_FROM_ALL,
    # so the main `ninja` above did not build it — and it could not have: its sysroot
    # libc++ headers/.so only exist after build_libcxx.sh ran. Build it now (ninja target
    # libcxx_smoke_elf), after libc++ is installed and before mkdisk's manifest check.
    echo "=== Building libc++ smoke ELF ==="
    ninja -C build libcxx_smoke_elf
fi

# 3. seatd/libseat are core terminal dependencies. The remaining projects stay
# optional wlroots prerequisites.
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
    sed -e "s#@CC@#$CC_BIN#g" -e "s#@CXX@#$CXX_BIN#g" -e "s#@PYTHON@#$PYTHON_BIN#g" \
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
        local setup=(--prefix /usr --libdir lib --buildtype=release \
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
                # must find libdisplay-info.so.4 or compositor startup fails.
                ln -sf libdisplay-info.so.0.4.0 "$SYSROOT/usr/lib/libdisplay-info.so.4"
                ln -sf libdisplay-info.so.4 "$SYSROOT/usr/lib/libdisplay-info.so"
                cp -a "$source/include/libdisplay-info" "$SYSROOT/usr/include/"
                install -m 644 "$build_dir/meson-private/libdisplay-info.pc" "$SYSROOT/usr/lib/pkgconfig/"
                ;;
            seatd)
                install -d "$SYSROOT/usr/lib/pkgconfig" "$SYSROOT/usr/include" "$SYSROOT/usr/bin"
                install -m 755 "$build_dir/libseat.so.1" "$SYSROOT/usr/lib/"
                ln -sf libseat.so.1 "$SYSROOT/usr/lib/libseat.so"
                install -m 755 "$build_dir/seatd" "$SYSROOT/usr/bin/"
                install -m 644 "$source/include/libseat.h" "$SYSROOT/usr/include/"
                install -m 644 "$build_dir/meson-private/libseat.pc" "$SYSROOT/usr/lib/pkgconfig/"
                ;;
            *)
                meson install -C "$build_dir" --no-rebuild --destdir "$SYSROOT"
                ;;
        esac
    }

    if [ "${BUILD_WLROOTS_DEPS:-0}" = "1" ]; then
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
    fi
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
    python3 - "$SYSROOT" "${BUILD_WLROOTS_DEPS:-0}" <<'PY'
import os
import shutil
import sys

sysroot = sys.argv[1]
libdir = os.path.join(sysroot, 'usr', 'lib')
staged = {
    'libseat.so': ['libseat.so', 'libseat.so.1'],
}
if sys.argv[2] == '1':
    staged.update({
        'libpixman-1.so': ['libpixman-1.so', 'libpixman-1.so.0', 'libpixman-1.so.0.46.4'],
        'libxkbcommon.so': ['libxkbcommon.so', 'libxkbcommon.so.0', 'libxkbcommon.so.0.13.2'],
        'libdisplay-info.so': ['libdisplay-info.so', 'libdisplay-info.so.4', 'libdisplay-info.so.0.4.0'],
    })
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

    # terminal now links libseat, so defer it until the external library exists.
    ninja -C build terminal_dyn_elf

    # These tests link the just-staged external libraries, so they cannot be
    # part of the initial CMake ALL target. This mirrors the post-Mesa EGL test.
    if [ "${BUILD_WLROOTS_DEPS:-0}" = "1" ] && \
       echo "$CMAKE_EXTRA" | grep -q "TEST=1"; then
        echo "=== Building wlroots prerequisite smoke ELFs ==="
        ninja -C build \
            test_pixman_smoke_dyn_elf \
            test_display_info_smoke_dyn_elf \
            test_xkbcommon_smoke_dyn_elf
    fi

# 3. Mesa cross-build (auto when products are missing; --mesa forces it).
if [ "${BUILD_MESA:-0}" = "1" ]; then
#    Softpipe first to validate the cross pipeline; switch to virgl by exporting
#    MESA_DRIVER=virgl (one-line option change, identical codegen — see step2.md).
#    Runs after the sysroot is populated (deps 2) and before mkdisk (so .so land in image).
#    NOTE: the C++ GLSL compiler needs a C++ stdlib in the sysroot — build libc++ first
#    (./build.sh --cxx) before the Mesa step can link the megadriver; see mesa_worklist.md §五.
#    Guard: libc++ is opt-in (--cxx); the cross-file's cpp_args/cpp_link_args reference it,
#    so without libc++.so in the sysroot meson/ninja fail with C++ stdlib symbol errors
#    (or fall back to host libstdc++ → glibc dep). Fail early with a clear message instead.
if [[ ! -f build/sysroot/usr/lib/libc++.so ]]; then
    echo "Mesa cross-build needs libc++ in the sysroot: run './build.sh --cxx' first (opt-in," >&2
    echo "installs libc++.so/libc++abi/libunwind + c++/v1 headers into build/sysroot)." >&2
    exit 1
fi
MESADIR=build/mesa
CROSS=build/mesa-cross.txt
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
if [[ ! -x "$MESA_VENV/bin/meson" ]]; then
    python3 -m venv "$MESA_VENV"
    "$MESA_VENV/bin/pip" install -q --upgrade pip
    "$MESA_VENV/bin/pip" install -q "meson>=1.4" mako packaging pyyaml
fi
export PATH="$MESA_VENV/bin:$PATH"
PYTHON_BIN="$(cd "$MESA_VENV" && pwd)/bin/python3"

# Generate the cross-file (CC/CXX/PYTHON/SYSROOT absolute). venv must exist first (above).
# SYSROOT is the same build/sysroot install-headers/ls populates; the cross-file uses it
# for the compiler/linker --sysroot flag AND pkg_config_libdir/sys_root.
SYSROOT="$(cd build/sysroot && pwd)"
sed -e "s#@CC@#$CC_BIN#g" -e "s#@CXX@#$CXX_BIN#g" -e "s#@PYTHON@#$PYTHON_BIN#g" \
    -e "s#@SYSROOT@#$SYSROOT#g" \
    build_script/third_party/mesa/meson-cross-x86_64.txt.in > "$CROSS"

# Emit libdrm/zlib/expat/ffi .pc into the sysroot so meson dependency() resolves.
bash build_script/gen-pkgconfig.sh

GALLIUM="${MESA_DRIVER:-softpipe}"
MESA_SETUP=(
    "$MESADIR" third_party/mesa
    "-Dgallium-drivers=$GALLIUM"
    -Dglx=disabled -Dopengl=false -Dgles1=disabled -Dgles2=enabled
    -Degl=enabled -Dgbm=enabled
    -Dplatforms= -Dvulkan-drivers= -Dllvm=disabled
    -Dgallium-va=disabled -Dgallium-rusticl=false -Dvideo-codecs=
    -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled
    -Dspirv-tools=disabled
    --cross-file "$CROSS"
)
if [[ -d "$MESADIR" ]]; then
    # Cross-file compiler flags affect Meson's configure probes, but a normal
    # reconfigure retains failed probes. Only wipe when the cross-file changed.
    CROSS_STAMP="$MESADIR/.xos-cross-file.sha256"
    CROSS_SUM="$(sha256sum "$CROSS" | awk '{print $1}')"
    if [[ ! -f "$CROSS_STAMP" || "$(<"$CROSS_STAMP")" != "$CROSS_SUM" ]]; then
        meson setup --wipe "${MESA_SETUP[@]}"
        printf '%s\n' "$CROSS_SUM" > "$CROSS_STAMP"
    else
        meson setup --reconfigure "${MESA_SETUP[@]}"
    fi
else
    meson setup "${MESA_SETUP[@]}"
    sha256sum "$CROSS" | awk '{print $1}' > "$MESADIR/.xos-cross-file.sha256"
fi
ninja -C "$MESADIR"

# Stage Mesa's .so into build/ root (where the disk-image manifest + mkdisk expect
# them). meson introspect gives each target's build path. Mesa's versioned libs ship
# as REAL files named by their full version (e.g. libEGL.so.1.0.0) with soname
# (libEGL.so.1) and linker name (libEGL.so) normally symlinks — but FAT32 has no
# symlinks, so we stage all three as REAL copies in build/. mkdisk ships them to /lib/
# so the runtime loader resolves DT_NEEDED sonames (libEGL.so.1, libGLESv2.so.2, ...).
# Non-versioned libs (libgallium-26.1.4.so, dri_gbm.so) are single files, copied once.
python3 - "$MESADIR" <<'PY'
import json, os, shutil, sys
mesadir = sys.argv[1]
# Each entry: (output basename prefix the meson filename starts with, [names to stage]).
# Versioned libs: meson produces <base>.so.<version> as the real file; we stage real +
# soname (soversion) + linker name. Names verified from meson.build:
#   EGL   soversion=1 version=1.0.0 ; GLESv2 soversion=2 version=2.0.0
#   gbm   version=1.0.0 (soversion derived =1) ; gallium/dri_gbm unversioned (single file)
libs = {
    "libEGL.so":    ["libEGL.so",    "libEGL.so.1",    "libEGL.so.1.0.0"],
    "libGLESv2.so": ["libGLESv2.so", "libGLESv2.so.2", "libGLESv2.so.2.0.0"],
    "libgbm.so":    ["libgbm.so",    "libgbm.so.1",    "libgbm.so.1.0.0"],
    "libgallium-26.1.4.so": ["libgallium-26.1.4.so"],
    "dri_gbm.so":           ["dri_gbm.so"],
}
# Map each wanted real file to the meson target filename that produces it. meson's
# intro-targets.json `filename` is the real on-disk name (e.g. libEGL.so.1.0.0).
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
PY
fi

# The EGL test links against staged Mesa .so files, so it must run after the
# optional Mesa stage above rather than in the first CMake ninja invocation.
if echo "$CMAKE_EXTRA" | grep -q "TEST=1"; then
    echo "=== Building EGL/GLES2 smoke ELF ==="
    ninja -C build test_egl_smoke_elf
fi

# 4. Generate disk.img (single disk, two partitions: ESP + root FAT32)
TEST="${TEST:-0}"
if echo "$CMAKE_EXTRA" | grep -q "TEST=1"; then
    TEST=1
fi
export TEST

./build_script/mkdisk.sh
