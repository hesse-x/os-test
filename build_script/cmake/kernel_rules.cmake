# kernel_rules.cmake — add_kernel_object() 封装内核 OBJECT library 编译规则

function(add_kernel_object lib_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;ASM_SOURCES" ${ARGN})

    add_library(${lib_name} OBJECT ${ARG_SOURCES} ${ARG_ASM_SOURCES})

    target_include_directories(${lib_name} PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(${lib_name} PROPERTIES
        CXX_STANDARD 17
        POSITION_INDEPENDENT_CODE OFF
    )

    # Assembly files get -m64 (not -fPIE)
    if(ARG_ASM_SOURCES)
        set_source_files_properties(${ARG_ASM_SOURCES} PROPERTIES
            COMPILE_FLAGS "-m64"
        )
    endif()
endfunction()
