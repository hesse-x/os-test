# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.21)

if(CMAKE_TOOLCHAIN_FILE)
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(myos C CXX ASM)

# Export compile database for clang-tidy incremental naming checks (see doc/design/code_standard.md)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Common C flags (kernel uses C with -fPIE; user-space uses C/C++ with -fno-pie)
# Note: -mno-sse/-mno-sse2/-mno-mmx are kernel-only (applied via
# kernel_rules.cmake). User-space needs SSE for double/float in printf
# and general floating-point, so they must NOT be global.
#
# -nostdinc removes ALL default system header search paths (/usr/include
# AND gcc's bundled freestanding dir). We re-add the freestanding headers
# (std*.h: stdint.h, stddef.h, stdarg.h, ...) via -isystem so <stdint.h>
# still resolves, while host glibc headers (/usr/include/stdio.h etc.)
# stay out of the search path — the build is now truly host-independent.
# -isystem (not -I) marks them as system headers so -Werror does not fire
# on their internals. The freestanding dir is queried from the active compiler
# at configure time (not hardcoded) so it survives version changes and works
# for both gcc (…/gcc/<ver>/include) and clang (…/clang/<ver>/include).
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
    OUTPUT_VARIABLE GCC_FREESTANDING_INC
    OUTPUT_STRIP_TRAILING_WHITESPACE)

# ---- Shared compile-flag variables (single source of truth) ----
# These are target-agnostic groupings referenced by both the global
# CMAKE_<LANG>_FLAGS (inherited by CMake targets) AND the bare-gcc custom
# commands in user_rules.cmake (which do NOT inherit CMake variables, so they
# must reference these explicitly to stay in sync). Edit a flag here once and
# every build path — kernel OBJECT libs, libc.a, user ELF, ld.so — picks it up.
#
# FREESTANDING_FLAGS: freestanding runtime basics shared by all code paths.
# WARN_FLAGS: warning gate. Applied to kernel, libc.a, user ELF, and ld.so
# alike so unused/dead code is caught everywhere, not just where CMake targets
# happen to inherit CMAKE_C_FLAGS.
#
# Defined as CMake LISTS (the natural form for target_compile_options and the
# bare-gcc COMMAND lists in user_rules.cmake). When splicing into the
# CMAKE_<LANG>_FLAGS *string* below, string(JOIN) flattens the list with single
# spaces — without it the list's semicolons would leak into the string and
# break the compiler invocation.
# FREESTANDING_FLAGS: freestanding runtime basics. The kernel and EFI stub keep
# -isystem ${GCC_FREESTANDING_INC} (compiler-bundled std*.h) — musl headers break
# in-kernel builds (sigset_t/sigaction_t undefined, struct proc sizeof assertion
# fails). Userspace instead consumes musl's stdint/stddef/stdarg/stdbool via
# USER_FREESTANDING_FLAGS (+ musl/include on the search path in user_rules.cmake),
# making the user sysroot self-contained without the compiler freestanding dir.
# KERNEL_FREESTANDING_FLAGS retains -isystem; USER_FREESTANDING_FLAGS drops it.
# The FREESTANDING_FLAGS alias still equals the kernel variant so os_base_options
# (kernel/EFI, below) is unchanged.
set(KERNEL_FREESTANDING_FLAGS -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mno-red-zone -nostdinc -isystem ${GCC_FREESTANDING_INC})
set(USER_FREESTANDING_FLAGS  -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mno-red-zone -nostdinc)
set(FREESTANDING_FLAGS ${KERNEL_FREESTANDING_FLAGS})
set(WARN_FLAGS
    -Wall -Wextra -Werror
    # musl's public endian.h intentionally relies on C operator precedence.
    -Wno-bitwise-op-parentheses -Wno-shift-op-parentheses)

# Vendored code is always optimized at -O2, independently of the project build
# type. Third-party helpers append this after any per-config compiler defaults.
set(THIRD_PARTY_OPT_FLAGS -O2)

# ---- os_base_options: shared baseline INTERFACE lib ----
# third_party and in-repo targets link as needed to get the common freestanding
# basics (-ffreestanding/-nostdlib/-fno-builtin/-fno-stack-protector/-mno-red-zone/
# -nostdinc/-isystem/-m64) + build-type -O/-g (mirroring CMake's per-config
# defaults, replacing the manual USER_BUILD_FLAGS mirror in user_rules.cmake).
# Excludes WARN_FLAGS and -mno-sse (target-specific); those stay per-target to
# avoid leaking into third_party.
add_library(os_base_options INTERFACE)
set(_base_opts ${FREESTANDING_FLAGS} -m64)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _base_opts -g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    list(APPEND _base_opts -O3 -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    list(APPEND _base_opts -O2 -g -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    list(APPEND _base_opts -Os -DNDEBUG)
endif()
if(PERF)
    list(APPEND _base_opts -g -fno-omit-frame-pointer)
endif()
target_compile_options(os_base_options INTERFACE ${_base_opts})

# ---- os_user_base_options: userspace baseline INTERFACE lib ----
# Mirrors os_base_options but with USER_FREESTANDING_FLAGS (no -isystem) + -m64.
# Userspace freestanding std headers (stdint/stddef/stdarg/stdbool) come from
# musl exclusively; the compiler freestanding dir is no longer on the userspace
# search path. Available to CMake-managed in-repo userspace targets. Third-party
# targets use the fixed-O2 baseline below. bare-gcc custom-command paths are
# self-contained via USER_FREESTANDING_FLAGS + MUSL_INCLUDE_FLAGS (user_rules.cmake).
add_library(os_user_base_options INTERFACE)
set(_user_base_opts ${USER_FREESTANDING_FLAGS} -m64)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _user_base_opts -g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    list(APPEND _user_base_opts -O3 -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    list(APPEND _user_base_opts -O2 -g -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    list(APPEND _user_base_opts -Os -DNDEBUG)
endif()
if(PERF)
    list(APPEND _user_base_opts -g -fno-omit-frame-pointer)
    target_compile_definitions(os_user_base_options INTERFACE PERF)
endif()
target_compile_options(os_user_base_options INTERFACE ${_user_base_opts})

# Fixed-optimization baseline for real CMake third-party targets. Debug symbols
# and config defines still follow the build type; optimization always ends in -O2.
add_library(os_third_party_base_options INTERFACE)
set(_third_party_base_opts ${USER_FREESTANDING_FLAGS} -m64)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _third_party_base_opts -g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG)
elseif(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    list(APPEND _third_party_base_opts -DNDEBUG)
    if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        list(APPEND _third_party_base_opts -g)
    endif()
endif()
list(APPEND _third_party_base_opts ${THIRD_PARTY_OPT_FLAGS})
if(PERF)
    list(APPEND _third_party_base_opts -g -fno-omit-frame-pointer)
endif()
target_compile_options(os_third_party_base_options INTERFACE ${_third_party_base_opts})

# ---- os_uapi: UAPI contract headers INTERFACE lib ----
# Header-only lib carrying include/uapi → #include "xos/*.h" (and the
# linux/asm/compat subtrees). Consumers target_link_libraries(<t> PRIVATE os_uapi)
# to get the include path, replacing the prior root-scope
# include_directories(include/uapi). Zero compile/link behavior change.
# Note: bare-gcc custom commands (add_user_elf/dyn_elf/ldso/add_user_lib SHARED/
# crt0/third_party SHARED) do not inherit directory scope and already add
# -I.../include/uapi manually, so this only serves CMake-managed targets
# (kernel OBJECT libs / static user libs / boot_obj).
add_library(os_uapi INTERFACE)
target_include_directories(os_uapi INTERFACE ${CMAKE_SOURCE_DIR}/include/uapi)

# ---- os_warn: in-repo warning gate INTERFACE lib ----
# Carries WARN_FLAGS (-Wall -Wextra -Werror); kernel/boot/userspace targets link
# as needed. third_party does NOT link (its code disables warnings and stays out
# of our -Werror gate — see third_party_rules). Kept separate from os_base_options
# so third_party can pull freestanding basics without being force-opened into -Werror.
add_library(os_warn INTERFACE)
target_compile_options(os_warn INTERFACE ${WARN_FLAGS})

# ---- os_kernel_warn: kernel unused-parameter exemption INTERFACE lib ----
# Carries -Wno-unused-parameter. MUST be linked via INTERFACE lib AFTER os_warn,
# not via target_compile_options: CMake places a target's own compile_options
# before the usage requirements of linked libs, so a direct
# target_compile_options(-Wno-unused-parameter) would land BEFORE os_warn's
# -Wall -Wextra -Werror. gcc is sticky on -Wno-* (later -Wall does not reopen
# -Wunused-parameter), but clang processes -Wall/-Wextra in order and reopens
# -Wunused-parameter, overriding the earlier -Wno-unused-parameter and making
# the kernel syscall stubs' unusedN placeholder params fail under -Werror.
# Linking the INTERFACE lib after os_warn guarantees order (target_link_libraries
# expands INTERFACE libs' usage requirements in listed order).
add_library(os_kernel_warn INTERFACE)
target_compile_options(os_kernel_warn INTERFACE -Wno-unused-parameter)

# Phase 3 flag dedup: removes the root CMAKE_C_FLAGS/CMAKE_CXX_FLAGS string
# assembly. Historically WARN_FLAGS+FREESTANDING_FLAGS+-fPIE/-fno-pie+-std=gnu17
# were glued into the global string and inherited by CMake-managed targets
# (kernel OBJECT libs / boot_obj / static user libs). Now per-target:
#   - kernel/boot: target_link_libraries(os_base_options os_warn) + per-target
#     -fPIE -std=gnu17 (see kernel_rules.cmake / boot/CMakeLists.txt).
#   - static user libs: self-contained via add_user_lib static path's
#     USER_COMPILE_FLAGS (target_compile_options).
#   - bare-gcc custom commands (add_user_elf/dyn_elf/ldso/SHARED libc.so/crt0):
#     already reference FREESTANDING_FLAGS/WARN_FLAGS variables manually, do not
#     inherit the global string, unaffected.
# The global string reverts to the toolchain baseline (-m64, see
# toolchain-x86_64.cmake); no more directory-scope flag injection.
# Config flags (-O3 -DNDEBUG etc.) come from CMake's per-config defaults +
# os_base_options. Third-party targets override only optimization with their
# trailing -O2 while retaining debug symbols/config defines.

# KASAN sanitizer flags (applied only to kernel OBJECT libraries via kernel_rules.cmake)
# KASAN_SHADOW_OFFSET must match kasan.h's #define: gcc's -fsanitize=kernel-address
# uses this command-line macro to generate shadow-address arithmetic that the
# runtime (kasan.c) must agree with. Recomputed for VMA_BASE=0xFFFFFF8000000000
# so shadow(VMA_BASE) lands at PML4[503] base 0xFFFFFB8000000000.
if(SANITIZE)
  set(KASAN_CFLAGS "-fsanitize=kernel-address" "-DKASAN_SHADOW_OFFSET=0xDFFFFB9000000000ULL" "-DSANITIZER=1")
endif()

# Debug-mode flags (-g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG) no longer
# need a root CMAKE_C_FLAGS append: CMake-managed targets (kernel/boot) get them
# via os_base_options's Debug branch (linked above); bare-gcc commands and
# static user libs get them via USER_BUILD_FLAGS. See reface_cmake.md §3.3.

# Include project root for #include "arch/x64/..." etc.
include_directories(${CMAKE_SOURCE_DIR})
# UAPI contract headers (include/uapi → #include "xos/*.h") migrated to the
# os_uapi INTERFACE lib; consumers get them via
# target_link_libraries(<t> PRIVATE os_uapi). bare-gcc custom commands still add
# -I.../include/uapi manually, unaffected.

# Build rule wrappers
include(build_script/cmake/kernel_rules.cmake)
include(build_script/cmake/user_rules.cmake)
include(build_script/cmake/third_party_rules.cmake)
# Disk-image manifest machinery (reface_cmake.md §6 / §4.5). Included after the
# helpers because the helpers call os_image_path() internally; defined before
# any target is added that uses it.
include(build_script/cmake/image_rules.cmake)
# musl subproject (ldso.md Phase 0): build musl libc.so (fused loader) + crt as a
# standalone ./configure && make subproject, ADDITIVE to the hand-written stack.
# Downstream (hello_musl_dyn) consumes the `musl_libc` target + MUSL_LIB_DIR.
# musl_rules.cmake is the single entry point: it runs musl_generate_headers() +
# sets MUSL_DIR up top, builds loader/crt/`musl_libc`, then includes the 14 libc
# OBJECT libs (modules/{unistd,fcntl,socket,dl,dirent,mman,stdio,multibyte,wchar,
# pthread,string,math,stdlib,time}.cmake) that feed libc.a/libc.so via EXTRA_OBJS.
include(build_script/third_party/musl/musl_rules.cmake)

# Boot loader (standalone EFI application, compiled with GNU-EFI)
add_subdirectory(boot)

# Kernel (higher-half ELF)
add_subdirectory(arch/x64)
add_subdirectory(kernel)

# User-space (libc, drivers, shell, test programs)
add_subdirectory(user)
add_subdirectory(init)

# The Wayland desktop is the only display path and is part of every image.
add_subdirectory(user/compositor)

# Test targets (only when --test is enabled)
if(TEST)
    add_subdirectory(user/test)
endif()

# Link kernel ELF (managed link).
# Migrated from the script-mode do_link.cmake to add_executable + a rewritten
# CMAKE_C_LINK_EXECUTABLE:
#   - target_link_libraries(myos.elf PRIVATE kernel_obj) aggregates the six
#     OBJECT libs via the dependency graph; adding a kernel lib only needs a
#     target_link_libraries at its layer, the root link rule is untouched.
#   - The rewritten link rule forces bare ld (intercepting the gcc driver's
#     implicit crt0/libgcc/default-entry/self-relax, constraint C2), keeping
#     --no-relax (constraint C1: -fPIE small code model + higher-half VMA are
#     outside the GOTPCREL→LEA relaxation window).
# OBJECT lib objects enter the link: in CMake 3.21+, OBJECT libs passed via
# target_link_libraries appear in <LINK_LIBRARIES> (aggregated as
# $<TARGET_OBJECTS:...>). To guard against version-specific behavior, an
# explicit $<TARGET_OBJECTS:...> fallback is added — ld dedups duplicate
# objects (same object file path twice links once), equivalent to the golden
# object set. This fallback may be removed once §6.2 verifies it.
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/build_script/linker.ld)
set(BIN_FILE ${CMAKE_BINARY_DIR}/myos.elf)

# Kernel linker: clang→lld, gcc→GNU ld. Both support --no-relax + linker.ld;
# the kernel has no -flto, so the choice only pairs with the toolchain (lld with
# clang, ld with gcc, same origin).
if(OS_COMPILER STREQUAL "gcc")
    set(KERNEL_LINKER ld)
else()
    set(KERNEL_LINKER ld.lld)
endif()

# Force bare ld/lld (bypass the gcc driver's implicit inputs); rewrite the C
# link rule as ld + --no-relax + linker.ld. --no-relax pairs with -fPIE small
# code model: disables RIP-relative relax, keeping the absolute/PC-relative
# relocations the higher-half needs.
# OBJECT lib objects are aggregated into <OBJECTS> via target_link_libraries,
# but CMake's transitive aggregation of OBJECT libs is not recursive like
# STATIC: myos.elf PRIVATE links kernel_obj→driver_obj→bsd_obj→xcore_obj→
# xcore_mem→arch_x64, only directly-linked objects auto-join <OBJECTS>; deeper
# transitively-linked OBJECT lib objects are not all aggregated (in practice
# xcore_obj symbols like printk were missing). So $<TARGET_OBJECTS:...> lists
# all six libs explicitly as a fallback.
# The dependency graph (inter-layer PUBLIC link) still carries its usage-
# requirement transitive benefits (include/flag propagation via PUBLIC).
set(_kernel_perf_link_flags "")
if(PERF)
    set(_kernel_perf_link_flags "--build-id=sha1")
endif()
set(CMAKE_C_LINK_EXECUTABLE
    "${KERNEL_LINKER} -m elf_x86_64 --no-relax ${_kernel_perf_link_flags} -T ${LINKER_SCRIPT} <OBJECTS> -o <TARGET>")

add_executable(myos.elf)
# Aggregate the six OBJECT libs' objects: explicit $<TARGET_OBJECTS:...> ensures
# the link command has all kernel objects (OBJECT lib transitive aggregation is
# not recursive, see above). The dependency graph (kernel_obj PUBLIC driver_obj
# → ... → arch_x64) also establishes build-order deps + transitive includes;
# adding a kernel object on an existing layer chain needs no change here.
target_link_libraries(myos.elf PRIVATE kernel_obj)
target_sources(myos.elf PRIVATE
    $<TARGET_OBJECTS:arch_x64>
    $<TARGET_OBJECTS:xcore_mem>
    $<TARGET_OBJECTS:xcore_obj>
    $<TARGET_OBJECTS:bsd_obj>
    $<TARGET_OBJECTS:driver_obj>
    $<TARGET_OBJECTS:kernel_obj>
    $<TARGET_OBJECTS:fcntl_sync_check>)
set_target_properties(myos.elf PROPERTIES
    OUTPUT_NAME "myos.elf"
    LINK_DEPENDS "${LINKER_SCRIPT}")

# Kernel ELF lives at the ESP image root (partition 1). Register for the
# disk-image manifest (reface_cmake.md §6).
os_image_path(myos.elf myos.elf myos.elf PARTITION 1)

# Copy EFI bootloader to build/ root directory
add_custom_target(copy_boot ALL
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_BINARY_DIR}/boot/BOOTX64.EFI
        ${CMAKE_BINARY_DIR}/BOOTX64.EFI
    DEPENDS boot_bin
    COMMENT "Copying BOOTX64.EFI to build/"
)

# EFI bootloader lands at EFI/BOOT/BOOTX64.EFI in the ESP (partition 1).
os_image_path(copy_boot BOOTX64.EFI EFI/BOOT/BOOTX64.EFI PARTITION 1)

os_image_path(mesa_egl          libEGL.so              lib/libEGL.so              PARTITION 2)
os_image_path(mesa_egl_soname   libEGL.so.1            lib/libEGL.so.1            PARTITION 2)
os_image_path(mesa_gles2        libGLESv2.so           lib/libGLESv2.so           PARTITION 2)
os_image_path(mesa_gles2_soname libGLESv2.so.2         lib/libGLESv2.so.2         PARTITION 2)
os_image_path(mesa_gallium      libgallium-26.1.4.so   lib/libgallium-26.1.4.so   PARTITION 2)
os_image_path(mesa_gbm          libgbm.so              lib/libgbm.so              PARTITION 2)
os_image_path(mesa_gbm_soname   libgbm.so.1            lib/libgbm.so.1            PARTITION 2)
os_image_path(mesa_dri_gbm      dri_gbm.so             lib/dri_gbm.so             PARTITION 2)

os_image_path(pixman_lib          libpixman-1.so          lib/libpixman-1.so          PARTITION 2)
os_image_path(pixman_soname       libpixman-1.so.0        lib/libpixman-1.so.0        PARTITION 2)
os_image_path(xkbcommon_lib       libxkbcommon.so         lib/libxkbcommon.so         PARTITION 2)
os_image_path(xkbcommon_soname    libxkbcommon.so.0       lib/libxkbcommon.so.0       PARTITION 2)
os_image_path(display_info_lib    libdisplay-info.so      lib/libdisplay-info.so      PARTITION 2)
os_image_path(display_info_soname libdisplay-info.so.4    lib/libdisplay-info.so.4    PARTITION 2)

os_image_path(wlroots_lib libwlroots-0.20.so lib/libwlroots-0.20.so PARTITION 2)

# seatd/libseat are part of the default terminal session path.
os_image_path(libseat_lib         libseat.so              lib/libseat.so              PARTITION 2)
os_image_path(libseat_soname      libseat.so.1            lib/libseat.so.1            PARTITION 2)
os_image_path(seatd_bin           seatd                   usr/bin/seatd               PARTITION 2)
os_image_path(seat_session_smoke  seat-session-smoke      usr/bin/seat-session-smoke  PARTITION 2)
os_image_path(seat_protocol_neg   seat-protocol-negative  usr/bin/seat-protocol-negative PARTITION 2)

# Write the disk-image manifest now that every target above has registered.
# (reface_cmake.md §6: single source of truth for mkdisk.sh.)
os_write_image_manifest()
