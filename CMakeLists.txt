cmake_minimum_required(VERSION 3.10)

if(CMAKE_TOOLCHAIN_FILE)
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(myos C CXX ASM)

# Common C++ flags (for kernel, not for boot/)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fPIE -mno-red-zone -mno-sse -mno-sse2 -mno-mmx")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -mno-sse -mno-sse2 -mno-mmx")

# Include project root for #include "arch/x64/..." etc.
include_directories(${CMAKE_SOURCE_DIR})

# Boot loader (独立 EFI 应用，用 GNU-EFI 编译)
add_subdirectory(boot)

# Kernel (higher-half ELF)
add_subdirectory(arch/x64)
add_subdirectory(kernel)
add_subdirectory(driver)

# 链接内核 ELF
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/arch/x64/linker.ld)
set(BIN_FILE ${CMAKE_BINARY_DIR}/myos.elf)
set(LINK_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/do_link.cmake)

add_custom_target(link ALL
    COMMAND ${CMAKE_COMMAND}
        -DOBJ_LIST="$<TARGET_OBJECTS:arch_x64>;$<TARGET_OBJECTS:kernel_mem>;$<TARGET_OBJECTS:kernel_obj>;$<TARGET_OBJECTS:driver_obj>"
        -DLINKER_SCRIPT=${LINKER_SCRIPT}
        -DBIN_FILE=${BIN_FILE}
        -P ${LINK_SCRIPT}
    BYPRODUCTS ${BIN_FILE}
    COMMENT "Linking myos.elf"
)

add_dependencies(link arch_x64 kernel_mem kernel_obj driver_obj)

# 将 EFI bootloader 复制到 build/ 根目录
add_custom_target(copy_boot ALL
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_BINARY_DIR}/boot/BOOTX64.EFI
        ${CMAKE_BINARY_DIR}/BOOTX64.EFI
    DEPENDS boot_bin
    COMMENT "Copying BOOTX64.EFI to build/"
)
