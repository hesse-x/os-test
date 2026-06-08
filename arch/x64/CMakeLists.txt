# arch/x64 object library

add_library(arch_x64 OBJECT
    start.S
    vectors.S
    trapentry.S
    ap_trampoline.S
    paging.cc
    trap.cc
    smp.cc
    apic.cc
)

target_include_directories(arch_x64 PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(arch_x64 PROPERTIES
    CXX_STANDARD 17
    POSITION_INDEPENDENT_CODE OFF
)

# Assembly files must NOT use -fPIE (they are raw assembly)
set_source_files_properties(start.S vectors.S trapentry.S ap_trampoline.S PROPERTIES
    COMPILE_FLAGS "-m64"
)
