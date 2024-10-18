file(GLOB CODE_SRC "*.c")
add_library(libc OBJECT ${CODE_SRC})
