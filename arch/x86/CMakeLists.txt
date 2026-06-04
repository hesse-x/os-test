# arch/x86 object library

add_library(arch_x86 OBJECT
    start.S
    vectors.S
    trapentry.S
    boot.cc
    paging.cc
    trap.cc
    lib.cc
)

target_include_directories(arch_x86 PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(arch_x86 PROPERTIES
    CXX_STANDARD 17
)

# C++ files use -fPIE (set via CMAKE_CXX_FLAGS), assembly files must NOT
set_source_files_properties(start.S vectors.S trapentry.S PROPERTIES
    COMPILE_FLAGS "-m32"
)
