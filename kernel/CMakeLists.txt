# kernel object library

add_subdirectory(mem)

add_library(kernel_obj OBJECT
    kernel.cc
    serial.cc
    trap.cc
    proc.cc
)

target_include_directories(kernel_obj PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(kernel_obj PROPERTIES
    CXX_STANDARD 17
)
