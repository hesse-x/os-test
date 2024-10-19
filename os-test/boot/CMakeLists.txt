add_library(boot_elf OBJECT bootsect.s)

add_executable(boot.bin)
target_link_libraries(boot.bin PRIVATE boot_elf)
target_link_options(boot.bin PRIVATE -Ttext 0x7c00)
target_link_options(boot.bin PRIVATE -Wl,-m,elf_i386)
target_link_options(boot.bin PRIVATE -Wl,--oformat,binary)
target_link_options(boot.bin PRIVATE -nostdlib -nostdinc)
