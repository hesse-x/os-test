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

set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -m32")

# set script path
set(BUILD_TOOLS "${CMAKE_SOURCE_DIR}/build_tools")
include(${BUILD_TOOLS}/macros.cmake)


# set include path
include_directories(${CMAKE_SOURCE_DIR})

add_subdirectory(os-test)

align_binary(
    NAME init
    SRC load_kernel.bin
    PATH os-test/init
    OUT init.bin
)

add_custom_command(
    OUTPUT os-image.bin
    ALL DEPENDS boot.bin kernel.bin init
    COMMAND ${CMAKE_COMMAND} -E cat $<TARGET_FILE:boot.bin> init.bin $<TARGET_FILE:kernel.bin> > os-image.bin
    COMMENT "Generate os-image"
)  

add_custom_target(os-image ALL DEPENDS os-image.bin)
