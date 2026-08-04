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

# Total disk size: 192MB = 393216 sectors
DISK_SECTORS=$((192 * 1024 * 1024 / 512))
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
mcopy -i "${BUILD_DIR}/part2.img" "${PROJECT_DIR}/third_party/libinput/quirks/10-generic-keyboard.quirks"  ::usr/share/libinput/
mcopy -i "${BUILD_DIR}/part2.img" "${PROJECT_DIR}/third_party/libinput/quirks/10-generic-mouse.quirks"     ::usr/share/libinput/

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

# ===================== libc++ (opt-in, probe-driven) =====================
# libc++ is built separately by build_libcxx.sh (./build.sh --cxx) into the
# sysroot; default build.sh does not build it. This is only a probe dispatch:
# if present in sysroot, copy to img /lib; if absent, skip silently (no error,
# no impact on img). Headers (include/c++/v1/*) already went in with the
# sysroot/usr/include tree mcopy -s above, no separate handling needed. The .so
# must be copied separately: libc++ ninja install produces .so.1.0 (real file)
# + .so.1/.so (symlink→1.0). FAT32 has no symlink support, so the .so.1.0 real
# file must be copied under each soname name (mirroring install-libs.sh's
# ld-musl-x86_64.so.1 dual-name handling: the runtime loader finds libc++.so.1
# via DT_NEEDED, link-time finds libc++.so, real content is in libc++.so.1.0 —
# all three names copied as real-file copies).
LIBCXX_LIB="${BUILD_DIR}/sysroot/usr/lib"
if [ -f "${LIBCXX_LIB}/libc++.so.1.0" ]; then
  # Three C++ runtime libs, each copied under soname real file + dev name (all
  # pointing at the .so.VERSION.0 real file).
  for lib in libc++ libc++abi libunwind; do
    # Find the lib's real major-version file (.so.1.0 form). Probe anchor is the
    # .so.1.0 real file (not a symlink), avoiding the host sysroot's .so-symlink
    # resolution ambiguity.
    real=$(ls "${LIBCXX_LIB}/${lib}.so."* 2>/dev/null | sort -V | tail -1 || true)
    if [ -n "$real" ] && [ -f "$real" ]; then
      base=$(basename "$real")   # e.g. libc++.so.1.0
      mcopy -i "${BUILD_DIR}/part2.img" "$real" "::lib/${base}"
      # Add dev name libc++.so and soname libc++.so.1 (FAT32 has no symlink → real-file copies).
      mcopy -i "${BUILD_DIR}/part2.img" "$real" "::lib/${lib}.so"
      mcopy -i "${BUILD_DIR}/part2.img" "$real" "::lib/${lib}.so.1" 2>/dev/null || true
      echo "  libc++: $base → /lib/$base (+ ${lib}.so, ${lib}.so.1)"
    fi
  done
else
  echo "  libc++: not built (run ./build.sh --cxx to enable) — skipping /lib/libc++*"
fi

# Mesa artifacts are registered in image_manifest.txt by CMake and copied in
# the manifest loop above. Do not copy them again here: mcopy rejects duplicate
# destination names and would abort disk creation after the files are present.

# Preserve root directory README
mcopy -i "${BUILD_DIR}/part2.img" "${TESTDATA_DIR}/README" ::README

# Write back FAT32 partition
dd if="${BUILD_DIR}/part2.img" of="${BUILD_DIR}/disk.img" bs=512 seek=${PART2_START} conv=notrunc status=none
rm -f "${BUILD_DIR}/part2.img"
