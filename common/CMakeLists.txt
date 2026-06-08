add_library(common_obj OBJECT
    elf.cc
)

target_include_directories(common_obj PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(common_obj PROPERTIES
    CXX_STANDARD 17
    POSITION_INDEPENDENT_CODE OFF
)
