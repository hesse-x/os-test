file(GLOB CODE_SRC "*.c")
add_library(kernel OBJECT ${CODE_SRC} kernel_entry.S)
add_subdirectory(interrupt)
add_subdirectory(mem)
