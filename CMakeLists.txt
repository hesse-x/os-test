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

# ---- os_base_options: 共性基础选项 INTERFACE 库（reface_cmake.md §3.3 / §4.7）----
# third_party 与仓库内 target 按需 link 获取共性 freestanding 基础项：
# -ffreestanding/-nostdlib/-fno-builtin/-fno-stack-protector/-mno-red-zone/-nostdinc/-isystem/-m64
# + 随 CMAKE_BUILD_TYPE 变化的 -O/-g（镜像 CMake 各 config 默认，替代 user_rules.cmake
#   的 USER_BUILD_FLAGS 手工镜像）。不含 WARN_FLAGS、不含 -mno-sse 等专属选项——
# those 由各 target 自身 target_compile_options 决定，避免泄漏进 third_party。
# 阶段 1 仅作为 third_party 的基础选项原语；阶段 3 将让仓库内 target 全面消费它
# 并拆除全局 CMAKE_C_FLAGS 字符串拼装。
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
target_compile_options(os_base_options INTERFACE ${_base_opts})

# ---- os_uapi: UAPI 契约头 INTERFACE 库（reface_cmake.md §4.7 阶段 2 试点）----
# 纯头文件库，承载 include/uapi → #include "xos/*.h"（及 linux/asm/compat 子树）。
# 消费方 target_link_libraries(<t> PRIVATE os_uapi) 取得头路径，替代阶段 2 前的
# 根目录作用域 include_directories(include/uapi)。零编译、零链接行为变更。
# 注意：bare-gcc custom-command（add_user_elf/dyn_elf/ldso/add_user_lib SHARED/crt0/
# third_party SHARED）不继承目录作用域，本就手工 -I.../include/uapi 自给自足，故
# 此处仅服务 CMake 托管 target（内核 OBJECT 库 / 静态用户库 / boot_obj）。
add_library(os_uapi INTERFACE)
target_include_directories(os_uapi INTERFACE ${CMAKE_SOURCE_DIR}/include/uapi)

# ---- os_warn: 仓库内 warning 门禁 INTERFACE 库（reface_cmake.md §3.3 阶段 3）----
# 承载 WARN_FLAGS（-Wall -Wextra -Werror），供内核/boot/用户态 target 按需 link 取得。
# third_party 不 link（其代码关 warning，不参与我方 -Werror 门禁，见 third_party_rules）。
# 与 os_base_options 分离：freestanding 基础项与 warning 门禁职责不同，避免 third_party
# 为取 freestanding 选项而被连带打开 -Werror。
add_library(os_warn INTERFACE)
target_compile_options(os_warn INTERFACE ${WARN_FLAGS})

# 阶段 3 flag 去重（reface_cmake.md §3.3/§5.3）：拆除根 CMAKE_C_FLAGS/CMAKE_CXX_FLAGS 字符串拼装。
# 历史上 WARN_FLAGS+FREESTANDING_FLAGS+-fPIE/-fno-pie+-std=gnu17 拼进全局字符串，CMake
# 托管 target（内核 OBJECT 库 / boot_obj / 静态用户库）经继承取得。现拆为 per-target：
#   - 内核/boot：target_link_libraries(os_base_options os_warn) + per-target -fPIE -std=gnu17
#     （见 kernel_rules.cmake / boot/CMakeLists.txt）。
#   - 静态用户库：经 add_user_lib 静态路径的 USER_COMPILE_FLAGS（target_compile_options）自给自足。
#   - bare-gcc custom-command（add_user_elf/dyn_elf/ldso/SHARED libc.so/crt0）：本就手工引用
#     FREESTANDING_FLAGS/WARN_FLAGS 变量，不继承全局字符串，不受影响。
# 全局字符串退回工具链基线（-m64，见 toolchain-x86_64.cmake），不再做目录作用域 flag 注入。
# config flag（-O3 -DNDEBUG 等）由 CMake 各 config 默认 + os_base_options 共同提供；
# 两者对 CMake target 会重复，gcc 接受重复 flag，无害（与 libc.a 双副本 USER_COMPILE_FLAGS 一致）。

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
# UAPI 契约头（include/uapi → #include "xos/*.h"）已迁移到 os_uapi INTERFACE 库，
# 消费方经 target_link_libraries(<t> PRIVATE os_uapi) 取得（reface_cmake.md §4.7 阶段 2）。
# bare-gcc custom-command 仍各自手工 -I.../include/uapi，不受此变更影响。

# Build rule wrappers
include(build_script/cmake/kernel_rules.cmake)
include(build_script/cmake/user_rules.cmake)
include(build_script/cmake/third_party_rules.cmake)

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

# Link kernel ELF（reface_cmake.md §4.1 托管链接，阶段 4）
# 从 script 模式 do_link.cmake 迁移到 add_executable + 重写 CMAKE_C_LINK_EXECUTABLE：
#   - target_link_libraries(myos.elf PRIVATE kernel_obj) 经依赖图聚合六个 OBJECT 库，
#     新增内核库只需在所属层 target_link_libraries 挂一处，根链接命令零改动。
#   - 重写链接规则强制裸 ld（拦截 gcc 驱动隐式塞 crt0/libgcc/默认入口/自身松弛，约束 C2），
#     保留 --no-relax（约束 C1：-fPIE 小码模型 + higher-half VMA 在 GOTPCREL→LEA 松弛窗口外）。
# OBJECT 库对象进入链接命令的归属：经 target_link_libraries 传递的 OBJECT 库，其对象在
# CMake 3.21+ 会出现在 <LINK_LIBRARIES> 中（以 $<TARGET_OBJECTS:...> 形式聚合）。但为防
# 版本行为差异，显式补 $<TARGET_OBJECTS:...> 兜底——重复对象 ld 会 dedup（同一对象文件路径
# 出现两次，ld 仅链接一次），与金标准对象集等价。此兜底在 §6.2 验证通过后可考虑移除。
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/build_script/linker.ld)
set(BIN_FILE ${CMAKE_BINARY_DIR}/myos.elf)

# 强制链接接器为裸 ld（绕开 gcc 驱动隐式输入），覆写 C 链接规则为 ld + --no-relax + linker.ld。
# OBJECT 库的对象经 target_link_libraries 聚合进 <OBJECTS>。但 CMake 对 OBJECT 库的传递聚合
# 不像 STATIC 库那样递归：myos.elf PRIVATE 链 kernel_obj→driver_obj→bsd_obj→xcore_obj→xcore_mem→arch_x64
# 中，只有直接 link 的对象会自动并入 <OBJECTS>；传递链上更深层的 OBJECT 库对象不会自动全部聚合
# （实测缺 printk 等 xcore_obj 符号）。故显式用 $<TARGET_OBJECTS:...> 列全六库对象兜底。
# 依赖图（层间 PUBLIC link）仍保留其 usage-requirement 传递收益（include/flag 经 PUBLIC 向下游传）。
set(CMAKE_C_LINK_EXECUTABLE
    "ld -m elf_x86_64 --no-relax -T ${LINKER_SCRIPT} <OBJECTS> -o <TARGET>")

add_executable(myos.elf)
# 聚合六个 OBJECT 库的对象：经 $<TARGET_OBJECTS:...> 显式列出，确保链接命令含全部内核对象
# （OBJECT 库传递聚合不递归，见上注释）。依赖图（kernel_obj PUBLIC driver_obj → ... → arch_x64）
# 同时建立构建顺序依赖 + 传递 include，新增内核对象若挂在现有层链上则无需改此处。
target_link_libraries(myos.elf PRIVATE kernel_obj)
target_sources(myos.elf PRIVATE
    $<TARGET_OBJECTS:arch_x64>
    $<TARGET_OBJECTS:xcore_mem>
    $<TARGET_OBJECTS:xcore_obj>
    $<TARGET_OBJECTS:bsd_obj>
    $<TARGET_OBJECTS:driver_obj>
    $<TARGET_OBJECTS:kernel_obj>)
set_target_properties(myos.elf PROPERTIES OUTPUT_NAME "myos.elf")

# Copy EFI bootloader to build/ root directory
add_custom_target(copy_boot ALL
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_BINARY_DIR}/boot/BOOTX64.EFI
        ${CMAKE_BINARY_DIR}/BOOTX64.EFI
    DEPENDS boot_bin
    COMMENT "Copying BOOTX64.EFI to build/"
)
