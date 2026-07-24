# kernel_rules.cmake — add_kernel_object() wraps kernel OBJECT library build rules

function(add_kernel_object lib_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;ASM_SOURCES" ${ARGN})

    add_library(${lib_name} OBJECT ${ARG_SOURCES} ${ARG_ASM_SOURCES})

    target_include_directories(${lib_name} PRIVATE ${CMAKE_SOURCE_DIR})
    # UAPI 契约头（include/uapi → #include "xos/*.h"）经 os_uapi 取得
    # （reface_cmake.md §4.7 阶段 2：替代根目录作用域 include/uapi）。
    target_link_libraries(${lib_name} PRIVATE os_uapi)
    # 阶段 3 flag 去重（reface_cmake.md §3.3）：freestanding 基础 + config(-O3/-g/...)
    # 经 os_base_options；WARN_FLAGS 门禁经 os_warn。替代历史 CMAKE_C_FLAGS 字符串继承。
    target_link_libraries(${lib_name} PRIVATE os_base_options os_warn)
    # 内核 C 代码模型：-fPIE（小码模型，higher-half 需 --no-relax 配套，见根 CMakeLists 托管链接规则）+
    # -std=gnu17。ASM 源不取 -fPIE（下方 ASM_SOURCES 分支仅给 -m64）。
    target_compile_options(${lib_name} PRIVATE -fPIE -std=gnu17)
    target_compile_definitions(${lib_name} PRIVATE __KERNEL__)

    if(PERF)
        target_compile_definitions(${lib_name} PRIVATE PERF)
    endif()
    target_compile_options(${lib_name} PRIVATE -Wno-unused-parameter)

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
