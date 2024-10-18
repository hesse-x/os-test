file(GLOB CODE_SRC "*.c")
file(GLOB ASM_SRC "*.s")

add_library(cpu OBJECT ${CODE_SRC})
add_library(cpu_asm OBJECT ${ASM_SRC})
