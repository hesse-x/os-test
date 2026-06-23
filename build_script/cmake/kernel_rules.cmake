# kernel_rules.cmake — add_kernel_object() 封装内核 OBJECT library 编译规则

function(add_kernel_object lib_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;ASM_SOURCES" ${ARGN})

    add_library(${lib_name} OBJECT ${ARG_SOURCES} ${ARG_ASM_SOURCES})

    target_include_directories(${lib_name} PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(${lib_name} PRIVATE __KERNEL__)
    target_compile_options(${lib_name} PRIVATE -Wno-unused-parameter)

    # KASAN sanitizer flags (kernel-only)
    if(KASAN_CFLAGS)
        target_compile_options(${lib_name} PRIVATE ${KASAN_CFLAGS})
    endif()

    set_target_properties(${lib_name} PROPERTIES
        POSITION_INDEPENDENT_CODE OFF
    )

    # Assembly files get -m64 (not -fPIE)
    if(ARG_ASM_SOURCES)
        set_source_files_properties(${ARG_ASM_SOURCES} PROPERTIES
            COMPILE_FLAGS "-m64"
        )
    endif()
endfunction()
