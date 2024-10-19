add_subdirectory(boot)
add_subdirectory(cpu)
add_subdirectory(drivers)
add_subdirectory(kernel)
add_subdirectory(libc)

add_executable(kernel.bin)
target_link_libraries(kernel.bin PRIVATE kernel_entry cpu cpu_asm drivers kernel libc)
target_link_options(kernel.bin PRIVATE -static -e kernel_start -Ttext 0x9000)
target_link_options(kernel.bin PRIVATE -Wl,-m,elf_i386)
target_link_options(kernel.bin PRIVATE -Wl,--oformat,binary)
target_link_options(kernel.bin PRIVATE -nostdlib -nostdinc -nodefaultlibs -fno-builtin -ffreestanding -nostartfiles)

