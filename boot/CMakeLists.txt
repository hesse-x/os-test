# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# boot/ - EFI bootloader stub (standalone low-address EFI application)

# GNU-EFI paths — discovered via find_*(reface_cmake.md §4.6)，替换硬编码 host 路径。
# 缺失则 FATAL_ERROR 带安装提示（gnuefi 包名），避免在缺少 GNU-EFI 的环境里
# 给出误导性的编译错误。find_path 找 efi.h 所在目录（stub.c 用 #include <efi.h>，
# 故返回 /usr/include/efi 而非 /usr/include）；find_file 找 crt0 对象与 EFI 链接脚本
# （.lds 不是库，find_library 只搜 .so/.a，故用 find_file）。
find_path(GNU_EFI_INCLUDE_DIR efi.h
    PATHS /usr/include/efi /usr/local/include/efi)
if(NOT GNU_EFI_INCLUDE_DIR)
    message(FATAL_ERROR
        "GNU-EFI headers not found (looked for efi.h in /usr/include/efi).\n"
        "  Install the gnuefi package: apt install libgnuefi-dev (Debian/Ubuntu).")
endif()
set(GNU_EFI_ARCH_INCLUDE_DIR ${GNU_EFI_INCLUDE_DIR}/x86_64)

find_file(GNU_EFI_CRT crt0-efi-x86_64.o
    PATHS /usr/lib /usr/local/lib)
if(NOT GNU_EFI_CRT)
    message(FATAL_ERROR
        "GNU-EFI crt0 not found (looked for crt0-efi-x86_64.o in /usr/lib).\n"
        "  Install the gnuefi package: apt install libgnuefi-dev (Debian/Ubuntu).")
endif()

find_file(GNU_EFI_LINKER_SCRIPT elf_x86_64_efi.lds
    PATHS /usr/lib /usr/local/lib)
if(NOT GNU_EFI_LINKER_SCRIPT)
    message(FATAL_ERROR
        "GNU-EFI linker script not found (looked for elf_x86_64_efi.lds in /usr/lib).\n"
        "  Install the gnuefi package: apt install libgnuefi-dev (Debian/Ubuntu).")
endif()
set(GNU_EFI_LIB_DIR /usr/lib)

# Compile stub.c into an object file
add_library(boot_obj OBJECT stub.c)

target_include_directories(boot_obj PRIVATE
    ${GNU_EFI_INCLUDE_DIR}
    ${GNU_EFI_ARCH_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}
)
# UAPI 契约头（include/uapi → <xos/elf.h>）经 os_uapi 取得
# （reface_cmake.md §4.7 阶段 2：替代根目录作用域 include/uapi）。
target_link_libraries(boot_obj PRIVATE os_uapi)
# 阶段 3 flag 去重（reface_cmake.md §3.3）：freestanding 基础 + config 经 os_base_options；
# WARN_FLAGS 门禁经 os_warn。替代历史 CMAKE_C_FLAGS 字符串继承。
target_link_libraries(boot_obj PRIVATE os_base_options os_warn)
# -fPIE + -std=gnu17（与内核一致）；下方 C_STANDARD 11 会把 -std 覆盖为 gnu11。
target_compile_options(boot_obj PRIVATE -fPIE -std=gnu17)

target_compile_options(boot_obj PRIVATE
    -fpic
    -fno-stack-check
    -fshort-wchar
    -mno-red-zone
    -maccumulate-outgoing-args
)

target_compile_definitions(boot_obj PRIVATE
    -DEFI_FUNCTION_WRAPPER
)

set_target_properties(boot_obj PROPERTIES
    C_STANDARD 11
    POSITION_INDEPENDENT_CODE OFF
)

# Link EFI shared object
set(BOOT_ELF_FILE ${CMAKE_CURRENT_BINARY_DIR}/boot.so)
set(BOOT_BIN_FILE ${CMAKE_CURRENT_BINARY_DIR}/BOOTX64.EFI)

add_custom_command(
    OUTPUT ${BOOT_ELF_FILE}
    COMMAND ld
        -nostdlib
        -znocombreloc
        -T ${GNU_EFI_LINKER_SCRIPT}
        -shared
        -Bsymbolic
        -L ${GNU_EFI_LIB_DIR}
        ${GNU_EFI_CRT}
        $<TARGET_OBJECTS:boot_obj>
        -o ${BOOT_ELF_FILE}
        -lefi
        -lgnuefi
    DEPENDS $<TARGET_OBJECTS:boot_obj>
    COMMENT "Linking EFI bootloader"
)

add_custom_target(boot_elf ALL DEPENDS ${BOOT_ELF_FILE})
add_dependencies(boot_elf boot_obj)

# objcopy to PE/COFF EFI application (same as the uefi branch)
add_custom_command(
    OUTPUT ${BOOT_BIN_FILE}
    COMMAND objcopy
        -j .text
        -j .sdata
        -j .data
        -j .rodata
        -j .dynamic
        -j .dynsym
        -j .rel.*
        -j .rela.*
        -j .reloc
        -j .symtab
        --subsystem=10
        --target=efi-app-x86_64
        ${BOOT_ELF_FILE}
        ${BOOT_BIN_FILE}
    DEPENDS ${BOOT_ELF_FILE}
    COMMENT "Converting bootloader to PE/COFF EFI application"
)

add_custom_target(boot_bin ALL DEPENDS ${BOOT_BIN_FILE})
add_dependencies(boot_bin boot_elf)
