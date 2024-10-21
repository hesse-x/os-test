add_library(load_kernel.o OBJECT init.c ${CMAKE_SOURCE_DIR}/os-test/utils/disk_io.c)
target_compile_options(load_kernel.o PRIVATE -Os)

add_executable(load_kernel.elf)
target_link_libraries(load_kernel.elf PRIVATE load_kernel.o)
target_link_options(load_kernel.elf PRIVATE -static -e load_kernel -Ttext 0x9000)
target_link_options(load_kernel.elf PRIVATE -Wl,--no-dynamic-linker -N)
target_link_options(load_kernel.elf PRIVATE -Wl,-m,elf_i386)
# target_link_options(load_kernel.elf PRIVATE -Wl,--oformat,binary)
target_link_options(load_kernel.elf PRIVATE -nostdlib -nostdinc)

add_custom_target(load_kernel.bin
  objcopy -O binary -j .text load_kernel.elf load_kernel.bin
  DEPENDS load_kernel.elf
  COMMENT "Extract load_kernel.bin"
)
