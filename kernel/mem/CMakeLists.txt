# kernel/mem object library

add_library(kernel_mem OBJECT
    alloc.cc
    user_mapping.cc
)

target_include_directories(kernel_mem PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(kernel_mem PROPERTIES
    CXX_STANDARD 17
    POSITION_INDEPENDENT_CODE OFF
)
