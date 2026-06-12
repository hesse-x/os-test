# user_rules.cmake — add_user_lib() / add_user_elf() 封装用户态编译规则

# 用户态公共编译 flags (CMake list, semicolon-separated)
set(USER_COMPILE_FLAGS -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 -mno-mmx)

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
endfunction()

# add_user_elf: 用户态 ELF（compile → objcopy → ld）
# 用法: add_user_elf(name [C] SOURCES source1 ... [LINK_LIBS lib1 ...])
function(add_user_elf elf_name)
    cmake_parse_arguments(ARG "C" "" "SOURCES;LINK_LIBS" ${ARGN})

    set(ELF_DIR ${CMAKE_BINARY_DIR})
    set(OBJ_FILE ${ELF_DIR}/${elf_name}.o)
    set(STRIPPED_OBJ_FILE ${ELF_DIR}/${elf_name}.stripped.o)
    set(ELF_FILE ${ELF_DIR}/${elf_name}.elf)

    # Step 1: compile
    set(COMPILE_DEPS "")
    foreach(src ${ARG_SOURCES})
        list(APPEND COMPILE_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/${src})
    endforeach()

    if(ARG_C)
        set(COMPILE_CMD ${CMAKE_C_COMPILER})
        set(COMPILE_FLAGS ${USER_COMPILE_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/user/include)
    else()
        set(COMPILE_CMD ${CMAKE_CXX_COMPILER})
        set(COMPILE_FLAGS ${USER_COMPILE_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/user/include)
    endif()

    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${COMPILE_CMD} ${COMPILE_FLAGS} -c ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCES} -o ${OBJ_FILE}
        DEPENDS ${COMPILE_DEPS}
        COMMENT "Compiling ${elf_name}.o"
    )

    # Step 2: objcopy (strip .note.gnu.property)
    add_custom_command(
        OUTPUT ${STRIPPED_OBJ_FILE}
        COMMAND objcopy --remove-section .note.gnu.property ${OBJ_FILE} ${STRIPPED_OBJ_FILE}
        DEPENDS ${OBJ_FILE}
        COMMENT "Stripping ${elf_name}.o"
    )

    # Step 3: ld
    set(LD_DEPS ${STRIPPED_OBJ_FILE})
    set(LD_ARGS ${STRIPPED_OBJ_FILE})

    if(ARG_LINK_LIBS)
        foreach(lib ${ARG_LINK_LIBS})
            list(APPEND LD_DEPS ${CMAKE_BINARY_DIR}/lib${lib}.a)
            list(APPEND LD_ARGS ${CMAKE_BINARY_DIR}/lib${lib}.a)
        endforeach()
    endif()

    add_custom_command(
        OUTPUT ${ELF_FILE}
        COMMAND ld -m elf_x86_64 -Ttext 0x400000 ${LD_ARGS} -o ${ELF_FILE}
        DEPENDS ${LD_DEPS}
        COMMENT "Linking ${elf_name}.elf"
    )

    add_custom_target(${elf_name}_elf ALL DEPENDS ${ELF_FILE})

    # 依赖声明
    if(ARG_LINK_LIBS)
        add_dependencies(${elf_name}_elf ${ARG_LINK_LIBS})
    endif()
endfunction()
