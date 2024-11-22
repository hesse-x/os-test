file(GLOB CODE_SRC "*.c")
add_library(kernel OBJECT kernel_entry.S ${CODE_SRC})
add_subdirectory(interrupt)
add_subdirectory(mem)
