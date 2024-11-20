file(GLOB CODE_SRC "*.c")
file(GLOB ASM_SRC "*.s")

add_library(interrupt OBJECT ${CODE_SRC})
add_library(interrupt_asm OBJECT ${ASM_SRC})
