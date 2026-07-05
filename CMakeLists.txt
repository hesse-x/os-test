cmake_minimum_required(VERSION 3.10)

if(CMAKE_TOOLCHAIN_FILE)
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(myos C CXX ASM)

# Common C flags (kernel uses C with -fPIE; user-space uses C/C++ with -fno-pie)
# Note: -mno-sse/-mno-sse2/-mno-mmx are kernel-only (applied via
# kernel_rules.cmake). User-space needs SSE for double/float in printf
# and general floating-point, so they must NOT be global.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fPIE -mno-red-zone -std=gnu17")
# C++ flags for user-space only (driver, shell, libc .cc files)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pie -mno-red-zone")

# KASAN sanitizer flags (applied only to kernel OBJECT libraries via kernel_rules.cmake)
if(SANITIZE)
  set(KASAN_CFLAGS "-fsanitize=kernel-address" "-DKASAN_SHADOW_OFFSET=0xDFFFFC0000000000ULL" "-DSANITIZER=1")
endif()

# Debug 模式：加调试信息 + 保留帧指针（栈回溯需要）+ 放行所有日志级别
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG")
endif()

# Include project root for #include "arch/x64/..." etc.
include_directories(${CMAKE_SOURCE_DIR})
# UAPI contract headers live in include/uapi/xos/ but are included as
# #include "xos/xxx.h" (matching the published /usr/include/xos/ form).
include_directories(${CMAKE_SOURCE_DIR}/include/uapi)

# 编译规则封装
include(build_script/cmake/kernel_rules.cmake)
include(build_script/cmake/user_rules.cmake)

# Boot loader (独立 EFI 应用，用 GNU-EFI 编译)
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

# 链接内核 ELF
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

# 将 EFI bootloader 复制到 build/ 根目录
add_custom_target(copy_boot ALL
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_BINARY_DIR}/boot/BOOTX64.EFI
        ${CMAKE_BINARY_DIR}/BOOTX64.EFI
    DEPENDS boot_bin
    COMMENT "Copying BOOTX64.EFI to build/"
)
