# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# boot/ - EFI bootloader stub (standalone low-address EFI application)

# GNU-EFI paths — discovered via find_* (replacing hardcoded host paths).
# FATAL_ERROR with install hint on missing, avoiding misleading compile errors.
# find_path locates efi.h (stub.c uses #include <efi.h>, so it returns
# /usr/include/efi not /usr/include); find_file locates crt0 object + EFI linker
# script (.lds is not a library, find_library only searches .so/.a, so find_file).
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
# UAPI contract headers (include/uapi → <xos/elf.h>) via os_uapi.
target_link_libraries(boot_obj PRIVATE os_uapi)
# Freestanding basics + config via os_base_options; WARN_FLAGS gate via os_warn.
# Replaces the legacy CMAKE_C_FLAGS string inheritance.
target_link_libraries(boot_obj PRIVATE os_base_options os_warn)
# -fPIE + -std=gnu17 (matches kernel); C_STANDARD 11 below overrides -std to gnu11.
target_compile_options(boot_obj PRIVATE -fPIE -std=gnu17)

target_compile_options(boot_obj PRIVATE
    -fpic
    -fno-stack-check
    -fshort-wchar
    -mno-red-zone
)

# -maccumulate-outgoing-args: gcc-only ABI hint (x86-64 default is already on);
# clang does not accept this flag, so gate it to gcc.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(boot_obj PRIVATE -maccumulate-outgoing-args)
endif()

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
