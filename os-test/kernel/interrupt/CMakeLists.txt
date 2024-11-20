file(GLOB CODE_SRC "*.c")

add_library(interrupt OBJECT ${CODE_SRC})
