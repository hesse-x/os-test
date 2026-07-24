# third_party_rules.cmake — add_third_party_lib() for third-party submodule libraries
#
# 自治边界（reface_cmake.md §3.4）：third_party target 住在我方构建里，但各自维护
# 编译选项、关闭 warning，只通过 link os_base_options 获取共性基础项。我方专属选项
# （WARN_FLAGS、-mno-sse 等）不泄漏进 third_party。
#
# 产物形态：
#   - 运行时库（drm/ffi/input/udev）缺省 SHARED-only：编 .so，脚本拷 img，关 warning。
#     消费者不静态链第三方代码、运行时经 ld.so 加载（见 §3.4 附 6）。
#   - Unity 例外（编译期符号依赖）：STATIC 标记，test ELF 编译期链入 unity。
#
# SHARED 实现选型（reface_cmake.md §4.2 验证结论）：
#   add_library(SHARED) 在本工具链下 *不可用* —— toolchain 设 CMAKE_SYSTEM_NAME=Generic，
#   Generic 平台不提供共享库规则，显式 add_library(x SHARED ...) 也被降级为 STATIC
#   （实测：build rule 为 C_STATIC_LIBRARY_LINKER）。故 SHARED 走分支 B：custom-command
#   产 .so（与 add_user_lib SHARED 同一机制），但 flag 源用 os_base_options + -w，不携带
#   我方 WARN_FLAGS(-Werror)，实现第三方 warning 自治。
#   STATIC（Unity）走真 add_library(STATIC)，正常可用。

# _tp_base_compile_flags(out_var)
# 镜像 os_base_options 的共性基础项（freestanding + -m64 + config -O/-g），以 list 形式
# 返回供 custom-command 的 bare-gcc 调用直接消费。custom-command 不继承 CMake target
# usage requirement，故需手工展开 os_base_options 等价 flag。与根 CMakeLists 的
# os_base_options 保持同步（单一逻辑：freestanding 基础 + build-type 优化/调试）。
function(_tp_base_compile_flags out_var)
    set(_flags ${FREESTANDING_FLAGS} -m64)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND _flags -g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        list(APPEND _flags -O3 -DNDEBUG)
    elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        list(APPEND _flags -O2 -g -DNDEBUG)
    elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        list(APPEND _flags -Os -DNDEBUG)
    endif()
    set(${out_var} "${_flags}" PARENT_SCOPE)
endfunction()

# add_third_party_lib(name [C] [STATIC] SOURCES ...
#                     [OUTPUT_NAME name]               — 产物 base 名（缺省 = name）
#                     [INCLUDE_DIRS dir ...]            — 私有编译期 include（本库源用）
#                     [INTERFACE_INCLUDE_DIRS dir ...]  — 暴露给消费者的 include
#                     [FLAGS "..."]                     — 第三方自身选项（-D/-Wno-*/-include 等）
#                     [SO_LINK_LIBS lib ...]            — SHARED 运行时库的 .so 依赖（记 DT_NEEDED）
#                     [GEN_HEADERS hdr ...]             — configure_file 产物，编译期依赖追踪
#                     [LINK_DEPS target ...])           — 编译前置依赖（如 fourcc 表生成 target）
# 缺省 SHARED-only 运行时库（分支 B：custom-command .so）；STATIC 标记 Unity 类编译期链入。
function(add_third_party_lib name)
    set(option_args STATIC C)
    set(one_args OUTPUT_NAME)
    set(multi_args SOURCES INCLUDE_DIRS INTERFACE_INCLUDE_DIRS FLAGS SO_LINK_LIBS GEN_HEADERS LINK_DEPS)
    cmake_parse_arguments(ARG "${option_args}" "${one_args}" "${multi_args}" ${ARGN})

    # 产物 base 名（缺省 = name）。SHARED → lib<output>.so，STATIC → lib<output>.a。
    if(ARG_OUTPUT_NAME)
        set(_output_name ${ARG_OUTPUT_NAME})
    else()
        set(_output_name ${name})
    endif()

    # 共性编译 flag：os_base_options 等价（freestanding + -m64 + config -O/-g），不含 WARN_FLAGS。
    _tp_base_compile_flags(_base_flags)

    # 私有 include：项目根（root-relative include 风格）+ UAPI 契约头 + user/include + 本库 INCLUDE_DIRS。
    set(_include_flags
        -I${CMAKE_SOURCE_DIR}
        -I${CMAKE_SOURCE_DIR}/include/uapi
        -I${CMAKE_SOURCE_DIR}/user/include)
    foreach(_dir ${ARG_INCLUDE_DIRS})
        list(APPEND _include_flags -I${_dir})
    endforeach()

    # 第三方自身选项。FLAGS 可能是字符串，转 list。-w 关 warning（不参与我方 -Werror 门禁）。
    separate_arguments(ARG_FLAGS_LIST UNIX_COMMAND "${ARG_FLAGS}")

    if(ARG_STATIC)
        # ---- STATIC（Unity 类）：真 add_library(STATIC)，正常可用 ----
        add_library(${name} STATIC ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE os_base_options)
        target_include_directories(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/include/uapi
            ${CMAKE_SOURCE_DIR}/user/include
            ${ARG_INCLUDE_DIRS})
        if(ARG_INTERFACE_INCLUDE_DIRS)
            target_include_directories(${name} INTERFACE ${ARG_INTERFACE_INCLUDE_DIRS})
        endif()
        target_compile_options(${name} PRIVATE -w ${ARG_FLAGS_LIST})
        set_target_properties(${name} PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
            OUTPUT_NAME ${_output_name})
    else()
        # ---- SHARED 运行时库（分支 B：custom-command .so，因 Generic 平台无 SHARED target）----
        if(ARG_C)
            set(COMPILE_CMD ${CMAKE_C_COMPILER})
            set(DEP_LANG "C")
        else()
            set(COMPILE_CMD ${CMAKE_CXX_COMPILER})
            set(DEP_LANG "CXX")
        endif()
        # -fPIC（.so 必需）+ -fvisibility=hidden（默认隐藏，仅 export 标记导出；个别库经 FLAGS 覆盖）。
        set(COMPILE_FLAGS_BASE ${_base_flags} ${_include_flags} -fPIC -fvisibility=hidden ${ARG_FLAGS_LIST} -w)

        set(OBJ_FILES "")
        set(idx 0)
        foreach(src ${ARG_SOURCES})
            if(src MATCHES "^/")
                set(src_full ${src})
            else()
                set(src_full ${CMAKE_CURRENT_SOURCE_DIR}/${src})
            endif()
            set(src_obj ${CMAKE_BINARY_DIR}/${name}_${idx}.o)
            add_custom_command(OUTPUT ${src_obj}
                COMMAND ${COMPILE_CMD} ${COMPILE_FLAGS_BASE} -c ${src_full} -o ${src_obj}
                DEPENDS ${src_full} ${ARG_GEN_HEADERS}
                IMPLICIT_DEPENDS ${DEP_LANG} ${src_full}
                COMMENT "Compiling ${name}_${idx}.o (third_party SHARED)")
            list(APPEND OBJ_FILES ${src_obj})
            math(EXPR idx "${idx} + 1")
        endforeach()

        set(SO_FILE ${CMAKE_BINARY_DIR}/lib${_output_name}.so)

        # 链依赖：对象 + .so 依赖（SO_LINK_LIBS → lib<dep>.so 文件）。
        set(SO_LINK_DEPS ${OBJ_FILES})
        set(SO_EXTRA_LDFLAGS "")
        if(ARG_SO_LINK_LIBS)
            list(APPEND SO_EXTRA_LDFLAGS -L${CMAKE_BINARY_DIR})
            foreach(_dep ${ARG_SO_LINK_LIBS})
                list(APPEND SO_LINK_DEPS ${CMAKE_BINARY_DIR}/lib${_dep}.so)
                list(APPEND SO_EXTRA_LDFLAGS -Wl,--no-as-needed -l${_dep})
            endforeach()
        endif()

        add_custom_command(OUTPUT ${SO_FILE}
            COMMAND gcc -shared -fPIC -nostdlib -nodefaultlibs
                    -Wl,--hash-style=gnu
                    -Wl,-soname,lib${_output_name}.so
                    ${SO_EXTRA_LDFLAGS}
                    -o ${SO_FILE} ${OBJ_FILES}
            DEPENDS ${SO_LINK_DEPS}
            COMMENT "Linking lib${_output_name}.so (third_party SHARED)")
        add_custom_target(${name} ALL DEPENDS ${SO_FILE})

        # SO_LINK_LIBS 的 .so 依赖 target（<dep>_so 或 lib<dep>_so 命名不一致，两者都试）。
        if(ARG_SO_LINK_LIBS)
            foreach(_dep ${ARG_SO_LINK_LIBS})
                foreach(_cand ${_dep}_so lib${_dep}_so)
                    if(TARGET ${_cand})
                        add_dependencies(${name} ${_cand})
                    endif()
                endforeach()
            endforeach()
        endif()

        # INTERFACE include 暴露给消费者：用一个 INTERFACE 库承载，消费者经 target_link_libraries
        # 取头（add_user_elf/add_user_dyn_elf 已读 INTERFACE_INCLUDE_DIRECTORIES）。
        if(ARG_INTERFACE_INCLUDE_DIRS)
            add_library(${name}_iface INTERFACE)
            target_include_directories(${name}_iface INTERFACE ${ARG_INTERFACE_INCLUDE_DIRS})
            # 让 ${name} 依赖 ${name}_iface（仅表达关联，iface 无构建产物）。
            add_dependencies(${name} ${name}_iface)
            # 消费者 link ${name} 时也应能拿到 iface 的 include —— 但 custom target 无
            # INTERFACE 属性。约定：需要 third_party 头的消费者额外 link ${name}_iface。
            # 现有调用方（drm_test_link/modetest 等）仍通过原 INCLUDE_DIRS 或 INTERFACE 路径取头，
            # 暂不强制改消费者（阶段 5 统一收口）。
        endif()
    endif()

    # 编译前置依赖（fourcc 表生成 / configure_file 产物的 target）。
    if(ARG_LINK_DEPS)
        add_dependencies(${name} ${ARG_LINK_DEPS})
    endif()
endfunction()
