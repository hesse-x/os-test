#!/bin/bash
# mkdisk.sh — generate single-disk two-partition disk.img
#
# Layout:
#   LBA 0:           MBR + partition table
#   Partition 1 (ESP): FAT16, ~32MB — UEFI boot files
#     /EFI/BOOT/BOOTX64.EFI
#     /myos.elf
#     /init.elf          ← stub loads into memory and passes to kernel (initrd-style)
#   Partition 2 (root):  FAT32, ~160MB — root file system
#     /driver/kbd.dev
#     /usr/bin/{terminal,shell,udevd}
#     /usr/include/         ← published header tree (install-headers.sh output:
#     │                        xos/ + sys/ + bits/ + std/unistd/time/fcntl +
#     │                        musl stdint/stddef/stdarg/stdbool). Self-contained
#     │                        so the OS can host native builds (gcc/Mesa) reading
#     │                        /usr/include with -nostdinc and no -isystem fallback.
#     /usr/lib/libc.a
#     /lib/{ld.so,libc.so,libinput.so,libudev.so,libm.so,libdrm.so,libffi.so,libexpat.so}
#     /local/{hello,hello_dyn}.elf
#     /test/*.elf + /test/lib/{liba,libb}.so
#     /usr/share/libinput/*.quirks
#     /README
#
# The kernel gets init.elf from boot_info to create the init process, no longer needs a raw LBA slot.
# The FAT32 driver parses the MBR partition table itself to find the root partition start LBA (fat32_init).
#
# Artifact list is driven by build/image_manifest.txt (CMake-generated, reface_cmake.md §6).
# Each manifest line: build_relpath<TAB>image_path<TAB>partition(1=ESP,2=root).
# Only static, non-build assets (libinput quirks, README) stay hardcoded below.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
TESTDATA_DIR="${PROJECT_DIR}/testdata"
MANIFEST="${BUILD_DIR}/image_manifest.txt"

# The manifest is generated at CMake configure time. If missing, the build was not
# configured (or an old build dir predates the manifest machinery).
if [ ! -f "${MANIFEST}" ]; then
    echo "mkdisk.sh: ${MANIFEST} not found, run build.sh first (CMake writes the image manifest)"
    exit 1
fi

# Dependency check: every build_relpath in the manifest must exist under build/.
# (mkdisk runs after ninja, so all artifacts are already built; this guards against
#  a stale manifest or a partially-built tree.)
while IFS=$'\t' read -r artifact image part; do
    # Skip comment / blank lines.
    [ -z "$artifact" ] && continue
    case "$artifact" in '#'*) continue ;; esac
    if [ ! -f "${BUILD_DIR}/${artifact}" ]; then
        echo "mkdisk.sh: ${BUILD_DIR}/${artifact} not found (manifest entry -> ${image}), run build.sh first"
        exit 1
    fi
done < "${MANIFEST}"

# Runtime contract assertion: the interactive shell, dynamic loader and libc
# must be in the image. Otherwise forkpty()->execlp("/bin/sh") fails at runtime
# with a bare 127 instead of a build-time error (terminal/step1.md §3.2).
for required in "bin/sh" "lib/ld-musl-x86_64.so.1" "lib/libc.so"; do
    if ! awk -F'\t' -v img="$required" '$2 == img { found=1 } END { exit !found }' "${MANIFEST}"; then
        echo "mkdisk.sh: required runtime file ${required} missing from image manifest"
        exit 1
    fi
done

# The default image includes LLVM/Clang and needs room for its shared libraries.
DISK_SECTORS=$((512 * 1024 * 1024 / 512))
# Partition 1 (ESP): 32MB = 65536 sectors, starts at LBA 2048 (1MB alignment)
PART1_START=2048
PART1_SECTORS=65536
# Partition 2 (root): remaining space
PART2_START=$((PART1_START + PART1_SECTORS))
PART2_SECTORS=$((DISK_SECTORS - PART2_START))

# Create zero-filled image
dd if=/dev/zero of="${BUILD_DIR}/disk.img" bs=512 count=${DISK_SECTORS} status=none

# Create MBR partition table
# Partition 1: ESP, type 0xEF (EFI), 1MB aligned — OVMF recognizes ESP by this type
# Partition 2: root, type 0x0C (W95 FAT32 LBA)
sfdisk "${BUILD_DIR}/disk.img" <<EOF
label: dos
unit: sectors

${BUILD_DIR}/disk.img1 : start=${PART1_START}, size=${PART1_SECTORS}, type=ef
${BUILD_DIR}/disk.img2 : start=${PART2_START}, size=${PART2_SECTORS}, type=0c
EOF

# ===================== Partition 1: ESP =====================
# Extract partition 1 region, format, write boot files, write back
dd if="${BUILD_DIR}/disk.img" of="${BUILD_DIR}/part1.img" bs=512 skip=${PART1_START} count=${PART1_SECTORS} status=none
# FAT16: a 32MB ESP has too few FAT32 clusters (mtools WARNING + OVMF refuses).
# UEFI spec allows FAT12/16/32; OVMF boots FAT16 ESP well.
mkfs.fat -F 16 -n ESP "${BUILD_DIR}/part1.img" >/dev/null

# Create parent directories for partition-1 entries (depth-ascending so parents
# precede children), then copy. image_root entries (parent == ".") skip mmd.
declare -A esp_dirs
esp_entries=()
while IFS=$'\t' read -r artifact image part; do
    [ -z "$artifact" ] && continue
    case "$artifact" in '#'*) continue ;; esac
    [ "$part" = "1" ] || continue
    esp_entries+=("${artifact}"$'\t'"${image}")
    p=$(dirname "$image")
    while [ "$p" != "." ] && [ -n "$p" ]; do esp_dirs["$p"]=1; p=$(dirname "$p"); done
done < "${MANIFEST}"
for d in $(printf '%s\n' "${!esp_dirs[@]}" | awk '{print gsub(/\//,"/"), $0}' | sort -n | cut -d' ' -f2-); do
    mmd -i "${BUILD_DIR}/part1.img" "::${d}"
done
for e in "${esp_entries[@]}"; do
    IFS=$'\t' read -r artifact image <<<"$e"
    mcopy -i "${BUILD_DIR}/part1.img" "${BUILD_DIR}/${artifact}" "::${image}"
done

dd if="${BUILD_DIR}/part1.img" of="${BUILD_DIR}/disk.img" bs=512 seek=${PART1_START} conv=notrunc status=none
rm -f "${BUILD_DIR}/part1.img"

# ===================== Partition 2: root file system =====================
dd if="${BUILD_DIR}/disk.img" of="${BUILD_DIR}/part2.img" bs=512 skip=${PART2_START} count=${PART2_SECTORS} status=none
mkfs.fat -F 32 -s 1 "${BUILD_DIR}/part2.img" >/dev/null
# Note: -s 1 (512B/cluster) at 64MB yields enough clusters (newer mtools requires ≥65525 clusters).

# Derive the full directory skeleton from partition-2 image_paths: collect every
# ancestor of every image_path, dedupe (associative array), then mmd in depth-
# ascending order so a parent always precedes its children (mtools mmd requires
# the parent to exist first).
declare -A root_dirs
root_entries=()
while IFS=$'\t' read -r artifact image part; do
    [ -z "$artifact" ] && continue
    case "$artifact" in '#'*) continue ;; esac
    [ "$part" = "2" ] || continue
    root_entries+=("${artifact}"$'\t'"${image}")
    p=$(dirname "$image")
    while [ "$p" != "." ] && [ -n "$p" ]; do root_dirs["$p"]=1; p=$(dirname "$p"); done
done < "${MANIFEST}"
for d in $(printf '%s\n' "${!root_dirs[@]}" | awk '{print gsub(/\//,"/"), $0}' | sort -n | cut -d' ' -f2-); do
    mmd -i "${BUILD_DIR}/part2.img" "::${d}"
done

# Copy every partition-2 build artifact into its image_path.
for e in "${root_entries[@]}"; do
    IFS=$'\t' read -r artifact image <<<"$e"
    mcopy -i "${BUILD_DIR}/part2.img" "${BUILD_DIR}/${artifact}" "::${image}"
done

# ===================== Static (non-build) assets =====================
# These are repo files, not build artifacts, so they stay explicit here (not in
# the manifest). Their target directories are not necessarily created by the
# manifest-driven skeleton above (no build artifact lands under usr/share/), so
# create them explicitly before the copy — mirroring the old hardcoded skeleton.
# usr/share may already exist when a manifest asset (for example wallpaper.png)
# created it. Force non-interactive collision handling: mtools otherwise opens
# /dev/tty for a prompt whose text is hidden by the stderr redirection.
mmd -D o -i "${BUILD_DIR}/part2.img" ::usr/share ::usr/share/libinput 2>/dev/null || true
shopt -s nullglob
quirks_sources=("${PROJECT_DIR}/third_party/libinput/quirks/"*.quirks)
if [ "${#quirks_sources[@]}" -eq 0 ]; then
    echo "mkdisk.sh: no libinput quirks found" >&2
    exit 1
fi
for required in 10-generic-mouse.quirks 30-vendor-qemu.quirks; do
    if [ ! -f "${PROJECT_DIR}/third_party/libinput/quirks/${required}" ]; then
        echo "mkdisk.sh: required libinput quirk ${required} is missing" >&2
        exit 1
    fi
done
mcopy -i "${BUILD_DIR}/part2.img" "${quirks_sources[@]}" ::usr/share/libinput/
shopt -u nullglob

# Audit the FAT image itself so copy/LFN failures cannot produce a silently
# broken image that libinput later reports only as "failed to find data files".
quirks_listing="$(mdir -i "${BUILD_DIR}/part2.img" -b ::usr/share/libinput/)"
quirks_count="$(printf '%s\n' "${quirks_listing}" | grep -c '\.quirks$' || true)"
if [ "${quirks_count}" -eq 0 ]; then
    echo "mkdisk.sh: FAT image contains no long .quirks filenames" >&2
    exit 1
fi
for required in 10-generic-mouse.quirks 30-vendor-qemu.quirks; do
    if ! printf '%s\n' "${quirks_listing}" | grep -q "/${required}$"; then
        echo "mkdisk.sh: FAT image audit missing ${required}" >&2
        exit 1
    fi
done
echo "mkdisk.sh: installed ${quirks_count} libinput quirks files"

# Install the xkeyboard-config closure needed by tinywl.
if grep -q $'\tusr/bin/tinywl\t2$' "${MANIFEST}"; then
    mmd -i "${BUILD_DIR}/part2.img" ::usr/share/X11 ::usr/share/X11/xkb \
        ::usr/share/X11/xkb/rules ::usr/share/X11/xkb/keycodes \
        ::usr/share/X11/xkb/symbols ::usr/share/X11/xkb/types \
        ::usr/share/X11/xkb/compat 2>/dev/null || true
    XKB_DATA="${PROJECT_DIR}/third_party/libxkbcommon/test/data"
    for section in rules keycodes symbols types compat; do
        mcopy -i "${BUILD_DIR}/part2.img" -s "${XKB_DATA}/${section}"/* \
            "::usr/share/X11/xkb/${section}/"
    done
fi

# Published header tree → /usr/include/ (standard FHS layout).
# install-headers.sh (run before mkdisk in build.sh) already published the
# self-contained header closure to build/sysroot/usr/include/ (xos/ + sys/ +
# bits/ + std/unistd/time/fcntl + musl freestanding stdint/stddef/stdarg/stdbool).
# Copy that whole tree into the image so the OS hosts native builds (gcc/Mesa)
# that read /usr/include with -nostdinc and no compiler -isystem fallback. Not a
# build artifact (it is a published aggregate), so it stays here rather than the
# manifest. mcopy -s recurses the source dir tree, creating image subdirs as needed.
SYSROOT_INC="${BUILD_DIR}/sysroot/usr/include"
if [ -d "${SYSROOT_INC}" ]; then
    mmd -i "${BUILD_DIR}/part2.img" ::usr/include 2>/dev/null || true
    mcopy -i "${BUILD_DIR}/part2.img" -s "${SYSROOT_INC}"/* "::usr/include/"
else
    echo "mkdisk.sh: ${SYSROOT_INC} missing — run install-headers.sh first (build.sh does this)." >&2
    exit 1
fi

# ===================== libc++ =====================
# libc++ is built separately by build_libcxx.sh into the sysroot as part of the
# default build. Require and copy it into img /lib. Headers (include/c++/v1/*)
# already went in with the
# sysroot/usr/include tree mcopy -s above, no separate handling needed. The .so
# must be copied separately: libc++ ninja install produces .so.1.0 (real file)
# + .so.1/.so (symlink→1.0). FAT32 has no symlink support, so the .so.1.0 real
# file must be copied under each soname name (mirroring install-libs.sh's
# ld-musl-x86_64.so.1 dual-name handling: the runtime loader finds libc++.so.1
# via DT_NEEDED, link-time finds libc++.so, real content is in libc++.so.1.0 —
# all three names copied as real-file copies).
LIBCXX_LIB="${BUILD_DIR}/sysroot/usr/lib"
# Three C++ runtime libs, each copied under soname real file + dev name (all
# pointing at the .so.VERSION.0 real file).
for lib in libc++ libc++abi libunwind; do
  real=$(ls "${LIBCXX_LIB}/${lib}.so."* 2>/dev/null | sort -V | tail -1 || true)
  if [ -z "$real" ] || [ ! -f "$real" ]; then
    echo "mkdisk.sh: required default runtime missing: ${LIBCXX_LIB}/${lib}.so.*" >&2
    exit 1
  fi
  base=$(basename "$real")   # e.g. libc++.so.1.0
  mcopy -i "${BUILD_DIR}/part2.img" "$real" "::lib/${base}"
  # FAT32 has no symlinks, so install the dev name and soname as real copies.
  mcopy -i "${BUILD_DIR}/part2.img" "$real" "::lib/${lib}.so"
  mcopy -i "${BUILD_DIR}/part2.img" "$real" "::lib/${lib}.so.1" 2>/dev/null || true
  echo "  libc++: $base → /lib/$base (+ ${lib}.so, ${lib}.so.1)"
done

# ===================== LLVM/Clang =====================
LLVM_ROOT="${BUILD_DIR}/sysroot/usr"
for required in bin/clang bin/ld.lld lib/libLLVM.so.18.1 \
    lib/libclang-cpp.so.18.1 lib/crt1.o lib/Scrt1.o lib/crti.o lib/crtn.o; do
  if [ ! -f "${LLVM_ROOT}/${required}" ]; then
    echo "mkdisk.sh: required target toolchain file missing: ${LLVM_ROOT}/${required}" >&2
    exit 1
  fi
done

mcopy -i "${BUILD_DIR}/part2.img" "${LLVM_ROOT}/bin/clang" "::usr/bin/clang"
mcopy -i "${BUILD_DIR}/part2.img" "${LLVM_ROOT}/bin/ld.lld" "::usr/bin/ld.lld"
mcopy -i "${BUILD_DIR}/part2.img" "${LLVM_ROOT}/lib/libclang-cpp.so.18.1" \
    "::lib/libclang-cpp.so.18.1"
for crt in crt1.o Scrt1.o crti.o crtn.o; do
  mcopy -i "${BUILD_DIR}/part2.img" "${LLVM_ROOT}/lib/${crt}" \
      "::usr/lib/${crt}"
done
mcopy -i "${BUILD_DIR}/part2.img" "${TESTDATA_DIR}/clang_smoke.c" \
    "::clang_smoke.c"

resource_dir="${LLVM_ROOT}/lib/clang/18/include"
runtime_dir="${LLVM_ROOT}/lib/clang/18/lib/linux"
if [ ! -d "$resource_dir" ] || [ ! -d "$runtime_dir" ]; then
  echo "mkdisk.sh: required clang resource directories are missing" >&2
  exit 1
fi
mmd -D o -i "${BUILD_DIR}/part2.img" ::usr/lib ::usr/lib/clang \
    ::usr/lib/clang/18 ::usr/lib/clang/18/include ::usr/lib/clang/18/lib \
    ::usr/lib/clang/18/lib/linux 2>/dev/null || true
mcopy -i "${BUILD_DIR}/part2.img" -s "$resource_dir"/* \
    "::usr/lib/clang/18/include/"
mcopy -i "${BUILD_DIR}/part2.img" "$runtime_dir"/* \
    "::usr/lib/clang/18/lib/linux/"
echo "  llvm: clang + lld + LLVM/Clang DSOs + crt + resource files → image"

# Mesa artifacts are registered in image_manifest.txt by CMake and copied in
# the manifest loop above. Do not copy them again here: mcopy rejects duplicate
# destination names and would abort disk creation after the files are present.

# Preserve root directory README
mcopy -i "${BUILD_DIR}/part2.img" "${TESTDATA_DIR}/README" ::README

# Keep the Clang sample available in every image, independent of build options.
mmd -i "${BUILD_DIR}/part2.img" ::tmp 2>/dev/null || true
mcopy -i "${BUILD_DIR}/part2.img" "${TESTDATA_DIR}/hello.c" ::tmp/hello.c

# Write back FAT32 partition
dd if="${BUILD_DIR}/part2.img" of="${BUILD_DIR}/disk.img" bs=512 seek=${PART2_START} conv=notrunc status=none
rm -f "${BUILD_DIR}/part2.img"
