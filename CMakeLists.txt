cmake_minimum_required(VERSION 3.17)
project(MyOS C ASM CXX)

# set freestanding
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding")
# set warning
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
# forbidden exception
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-exceptions")
# forbidden std
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin -fno-stack-protector -nodefaultlibs -fno-pic -nostartfiles")

# set arch and forbidden simd
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32 -mno-sse")

set(CMAKE_ASM_FLAGS "${CMAKE_ASM_NASM_FLAGS} -m32")

# set include path
include_directories(${CMAKE_SOURCE_DIR})

add_custom_command(
    OUTPUT os-image.bin
    COMMAND ${CMAKE_COMMAND} -E cat $<TARGET_FILE:boot.bin> $<TARGET_FILE:kernel.bin> > os-image.bin
    DEPENDS boot.bin kernel.bin
    COMMENT "Generate os-image"
)  

add_custom_target(os-image ALL DEPENDS os-image.bin)

add_subdirectory(os-test)
