# image_rules.cmake — disk-image manifest machinery (reface_cmake.md §6 / §4.5)
#
# mkdisk.sh consumes build/image_manifest.txt as the single source of truth for
# which build artifacts land in the disk image and where (reface_cmake.md §1.4
# north star: eliminate mkdisk as a second source of the artifact list).
#
# Format: one entry per line, TAB-separated, three columns:
#   build_relpath<TAB>image_path<TAB>partition(1=ESP,2=root)
# build_relpath is the path under ${CMAKE_BINARY_DIR}; image_path is the
# in-image destination (relative to the partition root); partition is 1 or 2.
# mkdisk reads this with `while IFS=$'\t' read` (bash-native).
#
# The manifest is written at configure time (not build time): helpers call
# os_image_path() to accumulate entries into a GLOBAL property (visible across
# add_subdirectory scopes), and the root CMakeLists.txt calls
# os_write_image_manifest() at the end to file(WRITE) the manifest. Reconfigure
# is triggered automatically by CMake when any CMakeLists.txt changes (ninja
# detects build.ninja stale → reconfigure → rewrite manifest), so the manifest
# never goes stale relative to the build graph.

# os_image_path(<target> <artifact_relpath> <image_dest> [PARTITION <1|2>])
#   Register one manifest entry for <target>'s build artifact.
#   - artifact_relpath: path under ${CMAKE_BINARY_DIR} (e.g. "myos.elf",
#     "libc.so", "test/pipe.elf").
#   - image_dest: in-image destination path (e.g. "EFI/BOOT/BOOTX64.EFI",
#     "lib/libc.so", "test/pipe.elf"). Parent of "." means image root.
#   - PARTITION: 1 = ESP, 2 = root (default 2).
#   <target> is accepted for readability/audit only (it does not need to be a
#   real CMake target — e.g. the copy_boot custom target).
function(os_image_path target artifact_relpath image_dest)
    cmake_parse_arguments(ARG "" "PARTITION" "" ${ARGN})
    if(NOT DEFINED ARG_PARTITION)
        set(ARG_PARTITION 2)
    endif()
    set_property(GLOBAL APPEND PROPERTY OS_IMAGE_MANIFEST_ENTRIES
        "${artifact_relpath}\t${image_dest}\t${ARG_PARTITION}")
endfunction()

# os_write_image_manifest() — called once at the end of the root CMakeLists.txt.
#   Reads the accumulated GLOBAL property, dedupes, and writes
#   ${CMAKE_BINARY_DIR}/image_manifest.txt.
function(os_write_image_manifest)
    get_property(_entries GLOBAL PROPERTY OS_IMAGE_MANIFEST_ENTRIES)
    list(REMOVE_DUPLICATES _entries)
    set(_path ${CMAKE_BINARY_DIR}/image_manifest.txt)
    file(WRITE ${_path}
        "# CMake-generated disk-image manifest: build_relpath<TAB>image_path<TAB>partition(1=ESP,2=root)\n")
    foreach(_e ${_entries})
        file(APPEND ${_path} "${_e}\n")
    endforeach()
    list(LENGTH _entries _n)
    message(STATUS "image_manifest: wrote ${_path} (${_n} entries)")
endfunction()
