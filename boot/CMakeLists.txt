# boot/ - EFI bootloader stub (独立低地址 EFI 应用)

# GNU-EFI 路径
set(GNU_EFI_INCLUDE_DIR /usr/include/efi)
set(GNU_EFI_ARCH_INCLUDE_DIR /usr/include/efi/x86_64)
set(GNU_EFI_LIB_DIR /usr/lib)
set(GNU_EFI_CRT /usr/lib/crt0-efi-x86_64.o)
set(GNU_EFI_LINKER_SCRIPT /usr/lib/elf_x86_64_efi.lds)

# 编译 stub.c 为目标文件
add_library(boot_obj OBJECT stub.c)

target_include_directories(boot_obj PRIVATE
    ${GNU_EFI_INCLUDE_DIR}
    ${GNU_EFI_ARCH_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}
)

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

# 链接 EFI 共享对象
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

# objcopy 转 PE/COFF EFI 应用（和 uefi 分支一致）
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
