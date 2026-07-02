# user_rules.cmake — add_user_lib() / add_user_elf() 封装用户态编译规则

# 用户态公共编译 flags (CMake list, semicolon-separated)
set(USER_COMPILE_FLAGS -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -mno-red-zone)

# 防御：用户态需要 SSE（double/printf/FPU 都依赖），任何 -mno-sse* 都是
# 全局 CMAKE_C_FLAGS 泄漏的信号（典型来源：内核-only flag 误放全局）。
# 检查 target 的 COMPILE_OPTIONS + 继承的 CMAKE_C_FLAGS，命中即 FATAL_ERROR。
function(user_assert_no_sse_disable target_name)
    # 收集 target 自身 options
    get_target_property(_opts ${target_name} COMPILE_OPTIONS)
    # 合并全局 C flags（CMake 把 CMAKE_C_FLAGS 当作 directory property）
    get_directory_property(_global_cflags COMPILE_OPTIONS)
    set(_all_flags ${_opts} ${_global_cflags})
    # CMAKE_C_FLAGS 是字符串，转成 list 用空格分
    separate_arguments(_global_c_flags UNIX_COMMAND "${CMAKE_C_FLAGS}")
    set(_all_flags ${_all_flags} ${_global_c_flags})
    foreach(_f ${_all_flags})
        if(_f MATCHES "^-mno-(sse|sse2|mmx)$")
            message(FATAL_ERROR
                "user-space target '${target_name}' got '${_f}' — SSE disabled.\n"
                "  user-space requires SSE for double/float/printf %f.\n"
                "  This usually means a kernel-only flag leaked into global\n"
                "  CMAKE_C_FLAGS. Move -mno-sse* to kernel_rules.cmake's\n"
                "  target_compile_options instead of the global flags.")
        endif()
    endforeach()
endfunction()

# add_user_lib: 用户态静态库（如 libc.a）
# 用法: add_user_lib(lib_name SOURCES source1 source2 ...)
function(add_user_lib lib_name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})

    add_library(${lib_name} STATIC ${ARG_SOURCES})

    target_include_directories(${lib_name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/user/include
    )

    target_compile_options(${lib_name} PRIVATE ${USER_COMPILE_FLAGS})

    # 输出到 build/ 根目录
    set_target_properties(${lib_name} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    user_assert_no_sse_disable(${lib_name})
endfunction()

# add_user_elf: 用户态 ELF（compile → objcopy → ld）
# 用法: add_user_elf(name [C] SOURCES source1 ... [LINK_LIBS lib1 ...] [DEFS def1 ...])
function(add_user_elf elf_name)
    cmake_parse_arguments(ARG "C" "" "SOURCES;LINK_LIBS;DEFS" ${ARGN})

    set(ELF_DIR ${CMAKE_BINARY_DIR})
    set(ELF_FILE ${ELF_DIR}/${elf_name}.elf)

    # Compiler selection
    if(ARG_C)
        set(COMPILE_CMD ${CMAKE_C_COMPILER})
    else()
        set(COMPILE_CMD ${CMAKE_CXX_COMPILER})
    endif()
    set(COMPILE_FLAGS ${USER_COMPILE_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/user/include -I${CMAKE_SOURCE_DIR}/third_party/Unity/src)

    # Extra compile definitions (-D flags)
    if(ARG_DEFS)
        foreach(def ${ARG_DEFS})
            list(APPEND COMPILE_FLAGS -D${def})
        endforeach()
    endif()

    # Determine dependency scanner language
    if(ARG_C)
        set(DEP_LANG "C")
    else()
        set(DEP_LANG "CXX")
    endif()

    # Step 1: compile each source file
    set(COMPILE_DEPS "")
    set(OBJ_FILES "")
    set(idx 0)
    foreach(src ${ARG_SOURCES})
        # Resolve path: if starts with /, use absolute; otherwise relative to CMAKE_CURRENT_SOURCE_DIR
        if(src MATCHES "^/")
            set(src_full ${src})
        else()
            set(src_full ${CMAKE_CURRENT_SOURCE_DIR}/${src})
        endif()

        set(src_obj ${ELF_DIR}/${elf_name}_${idx}.o)
        set(src_stripped ${ELF_DIR}/${elf_name}_${idx}.stripped.o)

        add_custom_command(
            OUTPUT ${src_obj}
            COMMAND ${COMPILE_CMD} ${COMPILE_FLAGS} -c ${src_full} -o ${src_obj}
            DEPENDS ${src_full}
            IMPLICIT_DEPENDS ${DEP_LANG} ${src_full}
            COMMENT "Compiling ${elf_name}_${idx}.o"
        )

        add_custom_command(
            OUTPUT ${src_stripped}
            COMMAND objcopy --remove-section .note.gnu.property ${src_obj} ${src_stripped}
            DEPENDS ${src_obj}
            COMMENT "Stripping ${elf_name}_${idx}.o"
        )

        list(APPEND COMPILE_DEPS ${src_full})
        list(APPEND OBJ_FILES ${src_stripped})
        math(EXPR idx "${idx} + 1")
    endforeach()

    # Step 2: ld
    set(LD_DEPS ${OBJ_FILES})
    set(LD_ARGS ${OBJ_FILES})

    if(ARG_LINK_LIBS)
        foreach(lib ${ARG_LINK_LIBS})
            list(APPEND LD_DEPS ${CMAKE_BINARY_DIR}/lib${lib}.a)
            list(APPEND LD_ARGS ${CMAKE_BINARY_DIR}/lib${lib}.a)
        endforeach()
    endif()

    add_custom_command(
        OUTPUT ${ELF_FILE}
        COMMAND ld -m elf_x86_64 -T ${CMAKE_SOURCE_DIR}/build_script/user_linker.ld ${LD_ARGS} -o ${ELF_FILE}
        DEPENDS ${LD_DEPS}
        COMMENT "Linking ${elf_name}.elf"
    )

    add_custom_target(${elf_name}_elf ALL DEPENDS ${ELF_FILE})

    # 依赖声明
    if(ARG_LINK_LIBS)
        add_dependencies(${elf_name}_elf ${ARG_LINK_LIBS})
    endif()
endfunction()

# add_user_ldso: ld.so 专用（-shared -fPIC，自带 minilibc，不链 libc.a）
# ld.md §3.4.4
function(add_user_ldso name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    set(ELF_FILE ${CMAKE_BINARY_DIR}/${name}.elf)
    set(COMPILE_FLAGS -m64 -ffreestanding -nostdlib -fno-builtin
                      -fPIC -fno-stack-protector -mno-red-zone
                      -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/user/include)
    set(OBJ_FILES "")
    set(idx 0)
    foreach(src ${ARG_SOURCES})
        if(src MATCHES "^/")
            set(src_full ${src})
        else()
            set(src_full ${CMAKE_CURRENT_SOURCE_DIR}/${src})
        endif()
        set(src_obj ${ELF_FILE}.${idx}.o)
        add_custom_command(OUTPUT ${src_obj}
            COMMAND gcc ${COMPILE_FLAGS} -c ${src_full} -o ${src_obj}
            DEPENDS ${src_full})
        list(APPEND OBJ_FILES ${src_obj})
        math(EXPR idx "${idx} + 1")
    endforeach()
    add_custom_command(OUTPUT ${ELF_FILE}
        COMMAND gcc -shared -fPIC -nostdlib -nodefaultlibs
                -Wl,-e,_start -Wl,--hash-style=gnu
                -o ${ELF_FILE} ${OBJ_FILES}
        DEPENDS ${OBJ_FILES}
        COMMENT "Linking ld.so (${name}.elf)")
    add_custom_target(${name}_elf ALL DEPENDS ${ELF_FILE})
endfunction()

# add_user_dyn_elf: 动态主 ELF，gcc driver 链接
# ld.md §3.4.4
# 阶段 2a 最小版本：仅生成 PT_INTERP + DT_NEEDED，不含完整 libc.so
function(add_user_dyn_elf name)
    cmake_parse_arguments(ARG "C" "" "SOURCES;LINK_LIBS" ${ARGN})
    set(ELF_FILE ${CMAKE_BINARY_DIR}/${name}.elf)
    set(COMPILE_CMD ${CMAKE_C_COMPILER})
    set(COMPILE_FLAGS ${USER_COMPILE_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/user/include)

    set(OBJ_FILES "")
    set(idx 0)
    foreach(src ${ARG_SOURCES})
        if(src MATCHES "^/")
            set(src_full ${src})
        else()
            set(src_full ${CMAKE_CURRENT_SOURCE_DIR}/${src})
        endif()
        set(src_obj ${ELF_FILE}.${idx}.o)
        add_custom_command(OUTPUT ${src_obj}
            COMMAND ${COMPILE_CMD} ${COMPILE_FLAGS} -c ${src_full} -o ${src_obj}
            DEPENDS ${src_full})
        list(APPEND OBJ_FILES ${src_obj})
        math(EXPR idx "${idx} + 1")
    endforeach()

    set(LD_ARGS ${OBJ_FILES})
    if(ARG_LINK_LIBS)
        foreach(lib ${ARG_LINK_LIBS})
            list(APPEND LD_ARGS -L${CMAKE_BINARY_DIR} -l${lib})
        endforeach()
    endif()
    # 生成空 stub 共享库，强制 gcc 生成动态可执行文件（PT_INTERP + PT_DYNAMIC）
    # 否则 -nostdlib + 无动态依赖时 ld 会丢弃 --dynamic-linker 指定的 .interp 段
    set(STUB_SO ${CMAKE_BINARY_DIR}/libdyn_stub.so)
    add_custom_command(OUTPUT ${STUB_SO}
        COMMAND gcc -shared -fPIC -nostdlib -o ${STUB_SO} -x c /dev/null
        COMMENT "Generating dyn stub shared library")
    add_custom_command(OUTPUT ${ELF_FILE}
        COMMAND gcc -fno-pie -no-pie
                -Wl,--dynamic-linker,/lib/ld.so
                -Wl,--hash-style=gnu
                -Wl,--no-as-needed
                -nostdlib -nodefaultlibs
                -o ${ELF_FILE} ${LD_ARGS} -L${CMAKE_BINARY_DIR} -ldyn_stub
        DEPENDS ${OBJ_FILES} ${ARG_LINK_LIBS} ${STUB_SO}
        COMMENT "Linking dynamic ${name}.elf")
    add_custom_target(${name}_dyn_elf ALL DEPENDS ${ELF_FILE})
endfunction()
