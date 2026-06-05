cmake_minimum_required(VERSION 3.10)

# Suppress "unused variable" warning for CMAKE_TOOLCHAIN_FILE — it is used
# implicitly by project() but CMake doesn't track that as a "use".
if(CMAKE_TOOLCHAIN_FILE)
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(myos C CXX ASM)

# Common C++ flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fPIE")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding -nostdlib")

# Include project root for #include "arch/x86/..." etc.
include_directories(${CMAKE_SOURCE_DIR})

# Collect object libraries from subdirectories
add_subdirectory(arch/x86)
add_subdirectory(kernel)
add_subdirectory(driver)

# Link step: write object list to a temp file with semicolons replaced by spaces,
# then execute the resulting command
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/arch/x86/linker.ld)
set(BIN_FILE ${CMAKE_BINARY_DIR}/myos.bin)
set(LINK_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/do_link.cmake)

add_custom_target(link ALL
    COMMAND ${CMAKE_COMMAND}
        -DOBJ_LIST="$<TARGET_OBJECTS:arch_x86>;$<TARGET_OBJECTS:kernel_mem>;$<TARGET_OBJECTS:kernel_obj>;$<TARGET_OBJECTS:driver_obj>"
        -DLINKER_SCRIPT=${LINKER_SCRIPT}
        -DBIN_FILE=${BIN_FILE}
        -P ${LINK_SCRIPT}
    BYPRODUCTS ${BIN_FILE}
    COMMENT "Linking myos.bin"
)

add_dependencies(link arch_x86 kernel_mem kernel_obj driver_obj)
