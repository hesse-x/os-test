# user_rules.cmake — add_user_lib() / add_user_elf() wrappers for userspace build rules

# Userspace common compile flags (CMake list, semicolon-separated)
# Bare-gcc custom commands (add_user_elf / add_user_ldso / SHARED libc.so /
# crt0) do NOT inherit global CMAKE_C_FLAGS, so they must carry these flags
# explicitly to resolve freestanding headers like <stdint.h> and to get the
# warning gate. They reference the shared FREESTANDING_FLAGS / WARN_FLAGS
# variables defined in CMakeLists.txt — the single source of truth — so a flag
# change there propagates here without manual mirroring. ( gcc accepts
# duplicate -nostdinc/-isystem, so this is also harmless for the CMake-target
# static-library path in add_user_lib, which gets the same flags via
# target_compile_options below. )
set(USER_COMPILE_FLAGS -m64 ${WARN_FLAGS} ${FREESTANDING_FLAGS} -fno-pie)

# DRM UAPI headers are infrastructure (display.h → "drm/drm.h"), so
# -I.../include is in base compile flags for project-wide "drm/drm.h"
# resolution. The upstream <drm.h> path (-I.../include/drm) and the
# xf86drm.h/xf86drmMode.h path (-I.../drm) are NOT global — they
# propagate via INTERFACE_INCLUDE_DIRECTORIES on the drm CMake target,
# so only targets that link libdrm.a (LINK_LIBS drm) inherit them.
set(DRM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/drm/include)

# Build-type flags for the bare-gcc commands below (add_user_elf / add_user_ldso /
# SHARED libc.so / add_user_dyn_elf). CMake targets (kernel OBJECT libs, static
# libc.a) inherit CMAKE_<LANG>_FLAGS_<CONFIG> automatically; these custom-command
# gcc invocations do NOT, so they would otherwise compile at -O0 even in Release
# and miss -g in Debug. Keep this in sync with CMake's defaults for each config.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(USER_BUILD_FLAGS -g -fno-omit-frame-pointer -DLOG_LEVEL_DEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(USER_BUILD_FLAGS -O3 -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(USER_BUILD_FLAGS -O2 -g -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(USER_BUILD_FLAGS -Os -DNDEBUG)
else()
    set(USER_BUILD_FLAGS "")
endif()

# Defense: userspace requires SSE (double/printf/FPU all depend on it), any
# -mno-sse* flag signals a leak from global CMAKE_C_FLAGS (typical source:
# kernel-only flag mistakenly placed globally).
# Check target's COMPILE_OPTIONS + inherited CMAKE_C_FLAGS, FATAL_ERROR on hit.
function(user_assert_no_sse_disable target_name)
    # Collect target's own options
    get_target_property(_opts ${target_name} COMPILE_OPTIONS)
    # Merge global C flags (CMake treats CMAKE_C_FLAGS as a directory property)
    get_directory_property(_global_cflags COMPILE_OPTIONS)
    set(_all_flags ${_opts} ${_global_cflags})
    # CMAKE_C_FLAGS is a string, convert to list splitting on whitespace
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

# add_user_lib: userspace library (libc.a static / libc.so shared)
# Usage: add_user_lib(name [C] SOURCES ... [FLAGS ...] [SHARED] [OUTPUT_NAME ...])
# C: flag to use C compiler (consistent with add_user_elf)
# SHARED: produce .so (gcc -shared -fPIC custom command, plan_ld2b3 decision 1 fallback)
# Otherwise: add_library(STATIC), preserve target interface (unity uses target_include_directories)
function(add_user_lib lib_name)
    set(option_args SHARED C)
    set(multi_args SOURCES FLAGS)
    set(one_args OUTPUT_NAME)
    cmake_parse_arguments(ARG "${option_args}" "${one_args}" "${multi_args}" ${ARGN})

    if(ARG_SHARED)
        # Shared library libc.so — add_library(SHARED) is unavailable in this toolchain, use custom command
        # plan_ld2b3 decision 1 fallback (same pattern as add_user_ldso)
        if(ARG_C)
            set(COMPILE_CMD ${CMAKE_C_COMPILER})
            set(DEP_LANG "C")
        else()
            set(COMPILE_CMD ${CMAKE_CXX_COMPILER})
            set(DEP_LANG "CXX")
        endif()
        # FLAGS may be a string (e.g. "-fno-pie -DDYNAMIC=0"), convert to list
        separate_arguments(ARG_FLAGS_LIST UNIX_COMMAND "${ARG_FLAGS}")
        # -fvisibility=hidden: default hidden, only LIBC_EXPORT-marked declarations are exported (consistent with ld.so).
        # .map + verify_libc_exports.sh is the final gate, catching missed/extra markings.
        set(COMPILE_FLAGS_BASE ${USER_COMPILE_FLAGS} ${USER_BUILD_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/include/uapi -I${CMAKE_SOURCE_DIR}/user/include -I${DRM_INCLUDE_DIR} -fvisibility=hidden ${ARG_FLAGS_LIST})

        set(OBJ_FILES "")
        set(idx 0)
        foreach(src ${ARG_SOURCES})
            if(src MATCHES "^/")
                set(src_full ${src})
            else()
                set(src_full ${CMAKE_CURRENT_SOURCE_DIR}/${src})
            endif()
            set(src_obj ${CMAKE_BINARY_DIR}/${lib_name}_${idx}.o)
            add_custom_command(OUTPUT ${src_obj}
                COMMAND ${COMPILE_CMD} ${COMPILE_FLAGS_BASE} -c ${src_full} -o ${src_obj}
                DEPENDS ${src_full}
                IMPLICIT_DEPENDS ${DEP_LANG} ${src_full}
                COMMENT "Compiling ${lib_name}_${idx}.o (SHARED)")
            list(APPEND OBJ_FILES ${src_obj})
            math(EXPR idx "${idx} + 1")
        endforeach()

        set(SO_FILE ${CMAKE_BINARY_DIR}/lib${ARG_OUTPUT_NAME}.so)
        add_custom_command(OUTPUT ${SO_FILE}
            COMMAND gcc -shared -fPIC -nostdlib -nodefaultlibs
                    -Wl,--hash-style=gnu
                    -Wl,-soname,lib${ARG_OUTPUT_NAME}.so
                    -Wl,--version-script,${CMAKE_SOURCE_DIR}/user/libc.map
                    -o ${SO_FILE} ${OBJ_FILES}
            COMMAND bash ${CMAKE_SOURCE_DIR}/build_script/cmake/verify_so_init_array.sh ${SO_FILE}
            COMMAND bash ${CMAKE_SOURCE_DIR}/build_script/cmake/verify_libc_exports.sh ${SO_FILE} ${CMAKE_SOURCE_DIR}/user/libc.map
            DEPENDS ${OBJ_FILES} ${CMAKE_SOURCE_DIR}/user/libc.map
            COMMENT "Linking ${lib_name}.so (libc.so)")
        add_custom_target(${lib_name} ALL DEPENDS ${SO_FILE})
    else()
        # Static library — add_library(STATIC), preserve target interface
        add_library(${lib_name} STATIC ${ARG_SOURCES})

        target_include_directories(${lib_name} PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/user/include
        )

        if(ARG_FLAGS)
            separate_arguments(ARG_FLAGS_LIST UNIX_COMMAND "${ARG_FLAGS}")
            target_compile_options(${lib_name} PRIVATE ${USER_COMPILE_FLAGS} ${ARG_FLAGS_LIST})
        else()
            target_compile_options(${lib_name} PRIVATE ${USER_COMPILE_FLAGS})
        endif()

        set_target_properties(${lib_name} PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        )

        user_assert_no_sse_disable(${lib_name})
    endif()
endfunction()

# add_drm_lib: third_party upstream library (static, relaxed warnings)
# Unlike add_user_lib which enforces WARN_FLAGS (-Werror etc), third_party
# submodule code should not carry our strict warning gate. This function
# shares only the essential compile infrastructure (architecture, freestanding,
# fno-pie) and common include paths; each library supplies its own -I list
# and extra -D/-include flags via INCLUDE_DIRS / FLAGS.
#
# INTERFACE_INCLUDE_DIRS are propagated to any target that links this library
# (add_user_elf/add_user_dyn_elf read INTERFACE_INCLUDE_DIRECTORIES from
# LINK_LIBS targets). This avoids polluting the global base compile flags with
# paths only needed by libdrm consumers.
# Usage: add_drm_lib(name [C] SOURCES ... [INCLUDE_DIRS dir1 ...] [FLAGS "..."]
#                       INTERFACE_INCLUDE_DIRS dir1 ...)
function(add_drm_lib lib_name)
    set(option_args C)
    set(multi_args SOURCES INCLUDE_DIRS FLAGS INTERFACE_INCLUDE_DIRS)
    cmake_parse_arguments(ARG "${option_args}" "" "${multi_args}" ${ARGN})

    add_library(${lib_name} STATIC ${ARG_SOURCES})

    # Common include paths every third_party lib needs (freestanding headers,
    # project-wide uapi, user/include). Library-specific headers go via
    # INCLUDE_DIRS so the lib controls its own resolution order.
    target_include_directories(${lib_name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include/uapi
        ${CMAKE_SOURCE_DIR}/user/include
        ${ARG_INCLUDE_DIRS}
    )

    # INTERFACE include dirs: propagated to consumers that link this library.
    # E.g. libdrm exposes xf86drm.h location + upstream <drm.h> resolution path.
    if(ARG_INTERFACE_INCLUDE_DIRS)
        target_include_directories(${lib_name} INTERFACE ${ARG_INTERFACE_INCLUDE_DIRS})
    endif()

    # Relaxed compile flags: -m64 + freestanding + -fno-pie, NO WARN_FLAGS.
    # Third-party upstream code is not subject to our -Werror gate; any
    # warning suppression must be passed explicitly via FLAGS.
    separate_arguments(ARG_FLAGS_LIST UNIX_COMMAND "${ARG_FLAGS}")
    target_compile_options(${lib_name} PRIVATE -m64 ${FREESTANDING_FLAGS} -fno-pie ${ARG_FLAGS_LIST})

    set_target_properties(${lib_name} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    user_assert_no_sse_disable(${lib_name})
endfunction()

# Build crt0.o (_start entry, linked into every static main ELF)
# plan_ld2b3 T5
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/crt0.o
    COMMAND gcc -ffreestanding -fno-pie -c ${CMAKE_SOURCE_DIR}/user/lib/crt0.S -o ${CMAKE_BINARY_DIR}/crt0.o
    DEPENDS ${CMAKE_SOURCE_DIR}/user/lib/crt0.S
    COMMENT "Compiling crt0.o")
add_custom_target(crt0_obj ALL DEPENDS ${CMAKE_BINARY_DIR}/crt0.o)

# add_user_elf: userspace ELF (compile → objcopy → ld)
# Usage: add_user_elf(name [C] SOURCES source1 ... [LINK_LIBS lib1 ...] [DEFS def1 ...])
function(add_user_elf elf_name)
    cmake_parse_arguments(ARG "C" "" "SOURCES;LINK_LIBS;DEFS;INCLUDE_DIRS" ${ARGN})

    set(ELF_DIR ${CMAKE_BINARY_DIR})
    set(ELF_FILE ${ELF_DIR}/${elf_name}.elf)

    # Compiler selection
    if(ARG_C)
        set(COMPILE_CMD ${CMAKE_C_COMPILER})
    else()
        set(COMPILE_CMD ${CMAKE_CXX_COMPILER})
    endif()
    set(COMPILE_FLAGS ${USER_COMPILE_FLAGS} ${USER_BUILD_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/include/uapi -I${CMAKE_SOURCE_DIR}/user/include -I${DRM_INCLUDE_DIR} -I${CMAKE_SOURCE_DIR}/third_party/Unity/src)

    # Extra include directories
    if(ARG_INCLUDE_DIRS)
        foreach(inc ${ARG_INCLUDE_DIRS})
            list(APPEND COMPILE_FLAGS -I${inc})
        endforeach()
    endif()

    # Propagate INTERFACE_INCLUDE_DIRECTORIES from linked library targets.
    # Bare-gcc custom commands don't inherit CMake target properties, so we
    # must read them manually. This mirrors target_link_libraries propagation
    # for static libs built with add_drm_lib (or any add_library that sets
    # INTERFACE include dirs).
    if(ARG_LINK_LIBS)
        foreach(lib ${ARG_LINK_LIBS})
            if(TARGET ${lib})
                get_target_property(_iface_includes ${lib} INTERFACE_INCLUDE_DIRECTORIES)
                if(_iface_includes)
                    foreach(inc ${_iface_includes})
                        list(APPEND COMPILE_FLAGS -I${inc})
                    endforeach()
                endif()
            endif()
        endforeach()
    endif()

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

    # Step 2: ld — crt0.o must be first in link input (provides _start)
    set(LD_DEPS ${CMAKE_BINARY_DIR}/crt0.o ${OBJ_FILES})
    set(LD_ARGS ${CMAKE_BINARY_DIR}/crt0.o ${OBJ_FILES})

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

    # Dependency declarations
    add_dependencies(${elf_name}_elf crt0_obj)
    if(ARG_LINK_LIBS)
        add_dependencies(${elf_name}_elf ${ARG_LINK_LIBS})
    endif()
endfunction()

# add_user_ldso: ld.so specific (-shared -fPIC, with built-in minilibc, does not link libc.a)
# ld.md §3.4.4
function(add_user_ldso name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    set(ELF_FILE ${CMAKE_BINARY_DIR}/${name}.elf)
    set(COMPILE_FLAGS -m64 ${WARN_FLAGS} ${FREESTANDING_FLAGS}
                      -fPIC -fvisibility=hidden
                      ${USER_BUILD_FLAGS}
                      -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/include/uapi -I${CMAKE_SOURCE_DIR}/user/include -I${DRM_INCLUDE_DIR})
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
        COMMAND bash ${CMAKE_SOURCE_DIR}/build_script/cmake/verify_ldso_rela_plt.sh ${ELF_FILE}
        DEPENDS ${OBJ_FILES}
        COMMENT "Linking ld.so (${name}.elf)")
    add_custom_target(${name}_elf ALL DEPENDS ${ELF_FILE})
endfunction()

# add_user_dyn_elf: dynamic main ELF, linked by gcc driver
# ld.md §3.4.4 / plan_ld2b3 T5
# crt0.o linked first (provides _start), libc.so linked via -L/-l (records DT_NEEDED)
function(add_user_dyn_elf name)
    cmake_parse_arguments(ARG "C" "" "SOURCES;LINK_LIBS;DEFS;INCLUDE_DIRS" ${ARGN})
    set(ELF_FILE ${CMAKE_BINARY_DIR}/${name}.elf)
    set(COMPILE_CMD ${CMAKE_C_COMPILER})
    set(COMPILE_FLAGS ${USER_COMPILE_FLAGS} ${USER_BUILD_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/include/uapi -I${CMAKE_SOURCE_DIR}/user/include -I${DRM_INCLUDE_DIR} -I${CMAKE_SOURCE_DIR}/third_party/Unity/src)

    # Extra include directories
    if(ARG_INCLUDE_DIRS)
        foreach(inc ${ARG_INCLUDE_DIRS})
            list(APPEND COMPILE_FLAGS -I${inc})
        endforeach()
    endif()

    # Propagate INTERFACE_INCLUDE_DIRECTORIES from linked library targets
    # (same logic as add_user_elf — see comment there).
    if(ARG_LINK_LIBS)
        foreach(lib ${ARG_LINK_LIBS})
            if(TARGET ${lib})
                get_target_property(_iface_includes ${lib} INTERFACE_INCLUDE_DIRECTORIES)
                if(_iface_includes)
                    foreach(inc ${_iface_includes})
                        list(APPEND COMPILE_FLAGS -I${inc})
                    endforeach()
                endif()
            endif()
        endforeach()
    endif()

    # Extra compile definitions (-D flags)
    if(ARG_DEFS)
        foreach(def ${ARG_DEFS})
            list(APPEND COMPILE_FLAGS -D${def})
        endforeach()
    endif()

    set(OBJ_FILES "")
    set(idx 0)
    foreach(src ${ARG_SOURCES})
        if(src MATCHES "^/")
            set(src_full ${src})
        else()
            set(src_full ${CMAKE_CURRENT_SOURCE_DIR}/${src})
        endif()
        set(src_obj ${ELF_FILE}.${idx}.o)
        if(ARG_C)
            set(DEP_LANG "C")
        else()
            set(DEP_LANG "CXX")
        endif()
        add_custom_command(OUTPUT ${src_obj}
            COMMAND ${COMPILE_CMD} ${COMPILE_FLAGS} -c ${src_full} -o ${src_obj}
            DEPENDS ${src_full}
            IMPLICIT_DEPENDS ${DEP_LANG} ${src_full})
        list(APPEND OBJ_FILES ${src_obj})
        math(EXPR idx "${idx} + 1")
    endforeach()

    # crt0.o must be first in link input (provides _start)
    set(LD_ARGS ${CMAKE_BINARY_DIR}/crt0.o ${OBJ_FILES})
    set(SO_DEPS "")
    if(ARG_LINK_LIBS)
        foreach(lib ${ARG_LINK_LIBS})
            # Dynamic ELF prefers .so (full path, avoids -lc mistakenly selecting libc.a)
            set(so_path ${CMAKE_BINARY_DIR}/lib${lib}.so)
            list(APPEND LD_ARGS ${so_path})
            list(APPEND SO_DEPS ${so_path})
        endforeach()
    endif()
    add_custom_command(OUTPUT ${ELF_FILE}
        COMMAND gcc -fno-pie -no-pie
                -Wl,--dynamic-linker,/lib/ld.so
                -Wl,--hash-style=gnu
                -Wl,--no-as-needed
                -Wl,--allow-shlib-undefined
                -nostdlib -nodefaultlibs
                -o ${ELF_FILE} ${LD_ARGS}
        DEPENDS ${OBJ_FILES} ${CMAKE_BINARY_DIR}/crt0.o ${SO_DEPS}
        COMMENT "Linking dynamic ${name}.elf")
    add_custom_target(${name}_dyn_elf ALL DEPENDS ${ELF_FILE})
    add_dependencies(${name}_dyn_elf crt0_obj)
endfunction()
