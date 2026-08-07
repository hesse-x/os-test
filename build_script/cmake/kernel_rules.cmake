# kernel_rules.cmake — add_kernel_object() wraps kernel OBJECT library build rules

function(add_kernel_object lib_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;ASM_SOURCES" ${ARGN})

    add_library(${lib_name} OBJECT ${ARG_SOURCES} ${ARG_ASM_SOURCES})

    target_include_directories(${lib_name} PRIVATE ${CMAKE_SOURCE_DIR})
    # UAPI contract headers (include/uapi → #include "xos/*.h") via os_uapi,
    # replacing the prior root-scope include_directories(include/uapi).
    target_link_libraries(${lib_name} PRIVATE os_uapi)
    # Phase 3 flag dedup: freestanding basics + config (-O3/-g/...) via
    # os_base_options; WARN_FLAGS gate via os_warn. -Wno-unused-parameter via
    # os_kernel_warn (MUST expand after os_warn — clang processes -Wall/-Wextra
    # in order and reopens -Wunused-parameter; see the root CMakeLists
    # os_kernel_warn comment).
    target_link_libraries(${lib_name} PRIVATE os_base_options os_warn os_kernel_warn)
    # Kernel C code model: -fPIE (small code model; higher-half needs --no-relax,
    # see the managed link rule in the root CMakeLists) + -std=gnu17. ASM sources
    # do not take -fPIE (the ASM_SOURCES branch below only gets -m64).
    # -fvisibility=hidden: marks C symbols STV_HIDDEN by default, shrinking the
    # symtab. The static kernel links in one pass; cross-object references and
    # asm .globl entry points are unaffected (resolved within the same link unit).
    target_compile_options(${lib_name} PRIVATE -fPIE -std=gnu17 -fvisibility=hidden)
    target_compile_definitions(${lib_name} PRIVATE __KERNEL__)

    if(PERF)
        target_compile_definitions(${lib_name} PRIVATE PERF)
    endif()
    if(TEST)
        target_compile_definitions(${lib_name} PRIVATE TEST)
    endif()
    # -Wno-unused-parameter moved to the os_kernel_warn INTERFACE lib (order-
    # sensitive, see comment above).

    # Kernel is built without SSE/SSE2/MMX: the x86-64 ABI would otherwise
    # pass/return double via XMM, and the kernel deliberately never touches
    # vector registers. User-space targets do NOT get these flags.
    target_compile_options(${lib_name} PRIVATE -mno-sse -mno-sse2 -mno-mmx)

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
