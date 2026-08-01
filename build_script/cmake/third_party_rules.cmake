# third_party_rules.cmake — add_third_party_lib() for third-party submodule libraries
#
# Autonomy boundary: third_party targets live in our build but each maintains its
# own compile options and disables warnings; they only pull common freestanding
# basics via link os_base_options. Our-specific options (WARN_FLAGS, -mno-sse, etc.)
# do not leak into third_party.
#
# Product forms:
#   - Runtime libs (drm/ffi/input/udev) default to SHARED-only: build .so, copy
#     to the image, disable warnings. Consumers do not statically link third-party
#     code; loaded at runtime via ld.so.
#   - Unity exception (compile-time symbol dependency): STATIC flag, test ELFs
#     link unity at compile time.
#
# SHARED implementation choice: add_library(SHARED) is *unavailable* under this
# toolchain — toolchain sets CMAKE_SYSTEM_NAME=Generic, and Generic provides no
# shared-library rules; even explicit add_library(x SHARED ...) is downgraded to
# STATIC (observed: build rule becomes C_STATIC_LIBRARY_LINKER). So SHARED uses
# branch B: a custom command produces .so (same mechanism as add_user_lib SHARED),
# but flags come from os_base_options + -w, without our WARN_FLAGS(-Werror),
# keeping third-party warning autonomy. STATIC (Unity) uses a real
# add_library(STATIC), which works normally.

# _tp_base_compile_flags(out_var)
# Mirrors os_base_options' common basics (freestanding + -m64 + config -O/-g),
# returned as a list for direct consumption by custom-command bare-gcc calls.
# Custom commands do not inherit CMake target usage requirements, so os_base_options'
# equivalent flags must be expanded manually. Kept in sync with the root
# CMakeLists os_base_options (single logic: freestanding basics + build-type
# optimization/debug).
function(_tp_base_compile_flags out_var)
    set(_flags ${USER_FREESTANDING_FLAGS} -m64)
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
#                     [OUTPUT_NAME name]               — artifact base name (default = name)
#                     [INCLUDE_DIRS dir ...]            — private compile-time include (for this lib's sources)
#                     [INTERFACE_INCLUDE_DIRS dir ...]  — include exposed to consumers
#                     [FLAGS "..."]                     — third-party's own options (-D/-Wno-*/-include etc.)
#                     [SO_LINK_LIBS lib ...]            — SHARED runtime lib .so deps (recorded as DT_NEEDED)
#                     [GEN_HEADERS hdr ...]             — configure_file outputs, compile-time dep tracking
#                     [LINK_DEPS target ...])           — compile prerequisite deps (e.g. fourcc table-gen target)
#                     [IMAGE_PATH dest] [IMAGE_ARTIFACT name] [IMAGE_PARTITION <1|2>] [NO_IMAGE]
# Default SHARED-only runtime lib (branch B: custom-command .so); STATIC for
# Unity-style compile-time linking.
# IMAGE_*: disk-image manifest registration. SHARED auto-defaults to
#   lib/lib<out>.so @ root; STATIC has no auto-default (compile-time linked, not
#   shipped separately). NO_IMAGE disables the SHARED default.
function(add_third_party_lib name)
    set(option_args STATIC C NO_IMAGE)
    set(one_args OUTPUT_NAME IMAGE_PATH IMAGE_ARTIFACT IMAGE_PARTITION)
    set(multi_args SOURCES INCLUDE_DIRS INTERFACE_INCLUDE_DIRS FLAGS SO_LINK_LIBS GEN_HEADERS LINK_DEPS)
    cmake_parse_arguments(ARG "${option_args}" "${one_args}" "${multi_args}" ${ARGN})

    # Artifact base name (default = name). SHARED → lib<output>.so, STATIC → lib<output>.a.
    if(ARG_OUTPUT_NAME)
        set(_output_name ${ARG_OUTPUT_NAME})
    else()
        set(_output_name ${name})
    endif()

    # IMAGE_PARTITION default = 2 (root); ESP placement must be explicit.
    if(NOT DEFINED ARG_IMAGE_PARTITION)
        set(ARG_IMAGE_PARTITION 2)
    endif()

    # Common compile flags: os_base_options equivalent (freestanding + -m64 +
    # config -O/-g), no WARN_FLAGS.
    _tp_base_compile_flags(_base_flags)

    # Private includes: project root (root-relative include style) + third_party
    # (musl shim: user/include/unistd.h does #include "musl/include/unistd.h",
    # resolved via -I third_party → third_party/musl/include/unistd.h) + UAPI
    # contract headers + user/include + musl headers (stdint/stddef/stdarg/stdbool
    # from musl since -isystem is dropped; user/include first so our
    # bits/alltypes.h wins. After pthread/signal/sched switched to musl,
    # third_party including <pthread.h>/<signal.h> lands in musl/include and pulls
    # <bits/alltypes.h>, so musl_gen's generated-header dir must precede it) +
    # this lib's INCLUDE_DIRS.
    set(_include_flags
        -I${CMAKE_SOURCE_DIR}
        -I${CMAKE_SOURCE_DIR}/third_party
        -I${CMAKE_SOURCE_DIR}/include/uapi
        -I${CMAKE_SOURCE_DIR}/user/include
        -I${CMAKE_BINARY_DIR}/musl_gen
        -I${CMAKE_SOURCE_DIR}/third_party/musl/include
        -I${CMAKE_SOURCE_DIR}/third_party/musl/arch/x86_64
        -I${CMAKE_SOURCE_DIR}/third_party/musl/arch/generic)
    foreach(_dir ${ARG_INCLUDE_DIRS})
        list(APPEND _include_flags -I${_dir})
    endforeach()

    # Third-party's own options. FLAGS may be a string, convert to list. -w
    # disables warnings (not subject to our -Werror gate).
    separate_arguments(ARG_FLAGS_LIST UNIX_COMMAND "${ARG_FLAGS}")

    if(ARG_STATIC)
        # ---- STATIC (Unity-style): real add_library(STATIC), works normally ----
        add_library(${name} STATIC ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE os_user_base_options)
        target_include_directories(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/third_party
            ${CMAKE_SOURCE_DIR}/include/uapi
            ${CMAKE_SOURCE_DIR}/user/include
            ${MUSL_GEN_DIR}
            ${CMAKE_SOURCE_DIR}/third_party/musl/include
            ${CMAKE_SOURCE_DIR}/third_party/musl/arch/x86_64
            ${CMAKE_SOURCE_DIR}/third_party/musl/arch/generic
            ${ARG_INCLUDE_DIRS})
        if(ARG_INTERFACE_INCLUDE_DIRS)
            target_include_directories(${name} INTERFACE ${ARG_INTERFACE_INCLUDE_DIRS})
        endif()
        target_compile_options(${name} PRIVATE -w ${ARG_FLAGS_LIST})
        set_target_properties(${name} PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
            OUTPUT_NAME ${_output_name})
        # Generated musl headers must exist before unity compiles (it pulls
        # musl headers via the include dirs above).
        add_dependencies(${name} musl_headers)
    else()
        # ---- SHARED runtime lib (branch B: custom-command .so, because the
        #      Generic platform has no SHARED target) ----
        if(ARG_C)
            set(COMPILE_CMD ${CMAKE_C_COMPILER})
            set(DEP_LANG "C")
        else()
            set(COMPILE_CMD ${CMAKE_CXX_COMPILER})
            set(DEP_LANG "CXX")
        endif()
        # -fPIC (required for .so) + -fvisibility=hidden (default hidden, only
        # export-marked declarations are exported; some libs override via FLAGS).
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
                DEPENDS ${src_full} ${ARG_GEN_HEADERS} ${MUSL_GEN_HEADERS}
                IMPLICIT_DEPENDS ${DEP_LANG} ${src_full}
                COMMENT "Compiling ${name}_${idx}.o (third_party SHARED)")
            list(APPEND OBJ_FILES ${src_obj})
            math(EXPR idx "${idx} + 1")
        endforeach()

        set(SO_FILE ${CMAKE_BINARY_DIR}/lib${_output_name}.so)

        # Link deps: objects + .so deps (SO_LINK_LIBS → lib<dep>.so files).
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
            COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -nostdlib -nodefaultlibs
                    -Wl,--hash-style=gnu
                    -Wl,-soname,lib${_output_name}.so
                    ${SO_EXTRA_LDFLAGS}
                    -o ${SO_FILE} ${OBJ_FILES}
            DEPENDS ${SO_LINK_DEPS}
            COMMENT "Linking lib${_output_name}.so (third_party SHARED)")
        add_custom_target(${name} ALL DEPENDS ${SO_FILE})

        # SO_LINK_LIBS .so dep targets (<dep>_so or lib<dep>_so naming differs,
        # try both).
        if(ARG_SO_LINK_LIBS)
            foreach(_dep ${ARG_SO_LINK_LIBS})
                foreach(_cand ${_dep}_so lib${_dep}_so)
                    if(TARGET ${_cand})
                        add_dependencies(${name} ${_cand})
                    endif()
                endforeach()
            endforeach()
        endif()

        # INTERFACE includes exposed to consumers: carried by an INTERFACE lib;
        # consumers get headers via target_link_libraries (add_user_elf/
        # add_user_dyn_elf already read INTERFACE_INCLUDE_DIRECTORIES).
        if(ARG_INTERFACE_INCLUDE_DIRS)
            add_library(${name}_iface INTERFACE)
            target_include_directories(${name}_iface INTERFACE ${ARG_INTERFACE_INCLUDE_DIRS})
            # Make ${name} depend on ${name}_iface (just expresses association;
            # iface has no build product).
            add_dependencies(${name} ${name}_iface)
            # Consumers linking ${name} should also get iface's includes — but
            # custom targets have no INTERFACE property. Convention: consumers
            # needing third_party headers additionally link ${name}_iface.
            # Existing call sites (drm_test_link/modetest etc.) still get headers
            # via the original INCLUDE_DIRS or INTERFACE path; consumers are not
            # force-changed yet (phase 5 will unify).
        endif()

        # Disk-image manifest: SHARED runtime .so auto-defaults to
        # lib/lib<out>.so @ root. NO_IMAGE disables the default; IMAGE_PATH/
        # IMAGE_ARTIFACT/IMAGE_PARTITION override.
        if(NOT ARG_NO_IMAGE)
            set(_img_artifact "lib${_output_name}.so")
            if(ARG_IMAGE_ARTIFACT)
                set(_img_artifact ${ARG_IMAGE_ARTIFACT})
            endif()
            set(_img_dest "lib/lib${_output_name}.so")
            if(ARG_IMAGE_PATH)
                set(_img_dest ${ARG_IMAGE_PATH})
            endif()
            os_image_path(${name} ${_img_artifact} ${_img_dest} PARTITION ${ARG_IMAGE_PARTITION})
        endif()
    endif()

    # Compile prerequisite deps (fourcc table generation / configure_file output targets).
    if(ARG_LINK_DEPS)
        add_dependencies(${name} ${ARG_LINK_DEPS})
    endif()
endfunction()
