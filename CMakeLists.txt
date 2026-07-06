# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.10)

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
# on their internals. The freestanding dir is queried from gcc at configure
# time (not hardcoded) so it survives gcc version changes.
execute_process(
    COMMAND gcc -print-file-name=include
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
set(FREESTANDING_FLAGS -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mno-red-zone -nostdinc -isystem ${GCC_FREESTANDING_INC})
set(WARN_FLAGS -Wall -Wextra -Werror)

# Flatten the lists into space-separated strings for CMAKE_<LANG>_FLAGS.
string(JOIN " " _FREESTANDING_STR ${FREESTANDING_FLAGS})
string(JOIN " " _WARN_STR ${WARN_FLAGS})

# Kernel + libc.a (CMake targets inherit CMAKE_C_FLAGS automatically).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_WARN_STR} ${_FREESTANDING_STR} -fPIE -std=gnu17")
# C++ flags for user-space only (driver, shell, libc .cc files). -Wall -Wextra
# are language-appropriate; -Werror is inherited via WARN_FLAGS.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_WARN_STR} ${_FREESTANDING_STR} -fno-pie")

# KASAN sanitizer flags (applied only to kernel OBJECT libraries via kernel_rules.cmake)
if(SANITIZE)
  set(KASAN_CFLAGS "-fsanitize=kernel-address" "-DKASAN_SHADOW_OFFSET=0xDFFFFC0000000000ULL" "-DSANITIZER=1")
endif()

# Debug mode: add debug info + preserve frame pointer (needed for stack backtrace) + allow all log levels
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG")
endif()

# Include project root for #include "arch/x64/..." etc.
include_directories(${CMAKE_SOURCE_DIR})
# UAPI contract headers live in include/uapi/xos/ but are included as
# #include "xos/xxx.h" (matching the published /usr/include/xos/ form).
include_directories(${CMAKE_SOURCE_DIR}/include/uapi)

# Build rule wrappers
include(build_script/cmake/kernel_rules.cmake)
include(build_script/cmake/user_rules.cmake)

# Boot loader (standalone EFI application, compiled with GNU-EFI)
add_subdirectory(boot)

# Kernel (higher-half ELF)
add_subdirectory(arch/x64)
add_subdirectory(kernel)

# User-space (libc, drivers, shell, test programs)
add_subdirectory(user)
add_subdirectory(init)

# Test targets (only when --test is enabled)
if(TEST)
    add_subdirectory(user/test)
endif()

# Link kernel ELF
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/build_script/linker.ld)
set(BIN_FILE ${CMAKE_BINARY_DIR}/myos.elf)
set(LINK_SCRIPT ${CMAKE_SOURCE_DIR}/build_script/cmake/do_link.cmake)

add_custom_target(link ALL
    COMMAND ${CMAKE_COMMAND}
        -DOBJ_LIST="$<TARGET_OBJECTS:arch_x64>;$<TARGET_OBJECTS:xcore_mem>;$<TARGET_OBJECTS:xcore_obj>;$<TARGET_OBJECTS:bsd_obj>;$<TARGET_OBJECTS:driver_obj>;$<TARGET_OBJECTS:kernel_obj>"
        -DLINKER_SCRIPT=${LINKER_SCRIPT}
        -DBIN_FILE=${BIN_FILE}
        -P ${LINK_SCRIPT}
    BYPRODUCTS ${BIN_FILE}
    COMMENT "Linking myos.elf"
)

add_dependencies(link arch_x64 xcore_mem xcore_obj bsd_obj driver_obj kernel_obj)

# Copy EFI bootloader to build/ root directory
add_custom_target(copy_boot ALL
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_BINARY_DIR}/boot/BOOTX64.EFI
        ${CMAKE_BINARY_DIR}/BOOTX64.EFI
    DEPENDS boot_bin
    COMMENT "Copying BOOTX64.EFI to build/"
)
