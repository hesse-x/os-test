add_library(load_kernel OBJECT init.c ${CMAKE_SOURCE_DIR}/os-test/utils/disk_io.c)
target_compile_options(load_kernel PRIVATE -Os)

add_executable(load_kernel.bin)
target_link_libraries(load_kernel.bin PRIVATE load_kernel)
target_link_options(load_kernel.bin PRIVATE -e load_kernel -Ttext 0x9000)
target_link_options(load_kernel.bin PRIVATE -N -Wl,-m,elf_i386)
target_link_options(load_kernel.bin PRIVATE -Wl,--oformat,binary)
target_link_options(load_kernel.bin PRIVATE -nostdlib -nostdinc)
