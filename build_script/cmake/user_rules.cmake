# user_rules.cmake — add_user_lib() / add_user_elf() wrappers for userspace build rules

# Userspace common compile flags (CMake list, semicolon-separated)
# Bare-gcc custom commands (add_user_elf / add_user_dyn_elf / SHARED libc.so)
# do NOT inherit global CMAKE_C_FLAGS, so they must carry these flags
# explicitly to resolve freestanding headers like <stdint.h> and to get the
# warning gate. They reference the shared USER_FREESTANDING_FLAGS / WARN_FLAGS
# variables defined in CMakeLists.txt — the single source of truth — so a flag
# change there propagates here without manual mirroring. ( gcc accepts
# duplicate -nostdinc, so this is also harmless for the CMake-target
# static-library path in add_user_lib, which gets the same flags via
# target_compile_options below. )
#
set(USER_COMPILE_FLAGS -m64 ${WARN_FLAGS} ${USER_FREESTANDING_FLAGS} -fno-pie)

# Userspace freestanding std headers (stdint/stddef/stdarg/stdbool) are provided
# by musl, NOT the compiler's -isystem freestanding dir (USER_FREESTANDING_FLAGS
# drops -isystem). MUSL_INCLUDE_FLAGS puts musl/include + arch on the search
# path. The generated-header directory supplies bits/alltypes.h and
# bits/syscall.h ahead of musl's .in templates. With pthread switched to musl,
# musl's
# <pthread.h>/<signal.h>/<sched.h> (the repo's own copies were deleted) also
# resolve from here.
set(MUSL_INCLUDE_FLAGS
    -I${CMAKE_SOURCE_DIR}/third_party/musl/include
    -I${CMAKE_SOURCE_DIR}/third_party/musl/arch/x86_64
    -I${CMAKE_SOURCE_DIR}/third_party/musl/arch/generic)

# When the kernel is built with KASAN (-DSANITIZE=1), propagate the SANITIZER
# macro to userspace too — WITHOUT -fsanitize=kernel-address (that is
# kernel-only; userspace has no shadow and the compiler's ASAN instrumentation
# would not link). The macro lets tests condition out cases whose kernel-side
# diagnostics trip KASAN under sanitizer builds (e.g. test_pthread_guard_pf:
# its intentional PROT_NONE #PF runs trap_dispatch's page-table-walk DIAG,
# which phys_to_virt's user-chosen physical addresses into the poisoned low
# shadow and halts the kernel on a KASAN false-positive). Userspace code
# otherwise behaves identically with or without this macro.
if(SANITIZE)
    list(APPEND USER_COMPILE_FLAGS -DSANITIZER=1)
endif()

# DRM UAPI headers are infrastructure (display.h → "drm/drm.h"), so
# -I.../include is in base compile flags for project-wide "drm/drm.h"
# resolution. The upstream <drm.h> path (-I.../include/drm) and the
# xf86drm.h/xf86drmMode.h path (-I.../drm) used to propagate via
# INTERFACE_INCLUDE_DIRECTORIES on the drm CMake target. Now that
# display.h directly includes xf86drm.h, -I.../drm is also global.
set(DRM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/drm/include)
# Additional DRM include paths: (1) <drm.h> resolution via include/drm,
# (2) xf86drm.h/xf86drmMode.h resolution via third_party/drm root.
# Used as separate -I flags via DRM_INCLUDE_FLAGS below.
set(DRM_XF86_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm)
set(DRM_XF86_INCLUDE_DIR2 ${CMAKE_SOURCE_DIR}/third_party/drm)
# Combined -I flags (used in compile flag construction below)
set(DRM_INCLUDE_FLAGS
    -I${DRM_INCLUDE_DIR}
    -I${DRM_XF86_INCLUDE_DIR}
    -I${DRM_XF86_INCLUDE_DIR2})

# Generated-musl-header paths used by the bare-gcc DEPENDS lines below and by
# add_musl_lib (pthread payload). musl_generate_headers() writes
# bits/alltypes.h / bits/syscall.h here; custom-command dependencies force
# generation before any musl-pulling compile.
set(MUSL_GEN_DIR ${CMAKE_BINARY_DIR}/musl_gen)
set(MUSL_GEN_HEADERS
    ${MUSL_GEN_DIR}/bits/alltypes.h
    ${MUSL_GEN_DIR}/bits/syscall.h)

# Build-type flags for the bare-gcc commands below (add_user_elf /
# add_user_dyn_elf / SHARED libc.so). CMake targets (kernel OBJECT libs, static
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
if(PERF)
    list(APPEND USER_COMPILE_FLAGS -DPERF)
    list(APPEND USER_BUILD_FLAGS -g -fno-omit-frame-pointer)
    set(USER_PERF_LINK_FLAGS -Wl,--build-id=sha1)
    set(USER_PERF_LD_FLAGS --build-id=sha1)
else()
    set(USER_PERF_LINK_FLAGS "")
    set(USER_PERF_LD_FLAGS "")
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

# _collect_iface_compile_definitions(out_var lib1 lib2 ...)
# Bare-gcc custom commands (add_user_elf/add_user_dyn_elf) do not inherit CMake
# target usage requirements, so to consume a linked library's PUBLIC/INTERFACE
# compile definitions we must read them manually. Walks each lib's
# INTERFACE_COMPILE_DEFINITIONS (transitively, via INTERFACE_LINK_LIBRARIES) and
# collects unique -D entries. This lets test ELF link `unity` (whose
# UNITY_EXCLUDE_MATH_H is now PUBLIC on the unity target)
# instead of re-declaring those DEFS at every call site.
function(_collect_iface_compile_definitions out_var)
    set(_seen "")
    set(_result "")
    set(_queue ${ARGN})
    while(_queue)
        list(POP_FRONT _queue _lib)
        # Guard against cycles / revisits.
        if(_lib IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen ${_lib})
        if(TARGET ${_lib})
            get_target_property(_defs ${_lib} INTERFACE_COMPILE_DEFINITIONS)
            if(_defs)
                foreach(_d ${_defs})
                    list(APPEND _result -D${_d})
                endforeach()
            endif()
            # Walk transitive link libraries (e.g. unity's own INTERFACE_LINK_LIBRARIES).
            get_target_property(_links ${_lib} INTERFACE_LINK_LIBRARIES)
            if(_links)
                foreach(_l ${_links})
                    if(NOT _l MATCHES "^[^;]+:")
                        list(APPEND _queue ${_l})
                    endif()
                endforeach()
            endif()
        endif()
    endwhile()
    set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

# add_user_lib: userspace library (libc.a static / libc.so shared / generic .so)
# Usage: add_user_lib(name [C] SOURCES ... [FLAGS ...] [SHARED] [OUTPUT_NAME ...]
#                     [VERSION_MAP ...] [SO_LINK_LIBS ...] [INCLUDE_DIRS ...]
#                     [GEN_HEADERS hdr1 ...]
#                     [EXTRA_OBJS obj1 ...]
#                     [IMAGE_PATH dest] [IMAGE_ARTIFACT name] [IMAGE_PARTITION <1|2>]
#                     [NO_IMAGE])
# C: flag to use C compiler (consistent with add_user_elf)
# SHARED: produce .so (gcc -shared -fPIC custom command, plan_ld2b3 decision 1 fallback)
# OUTPUT_NAME: custom base output name (without lib prefix); default = lib_name
# VERSION_MAP: path to version script (relative to CMAKE_SOURCE_DIR, e.g. user/libc.map)
#              Triggers --version-script + post-link export verification.
# SO_LINK_LIBS: additional .so dependencies for SHARED libs (e.g. "c" for libc.so)
#              Links -L${CMAKE_BINARY_DIR} -l<lib>.
# INCLUDE_DIRS: extra include directories (SHARED path only; static uses target_include_directories)
# GEN_HEADERS: generated headers (configure_file outputs in ${CMAKE_BINARY_DIR}, e.g.
#              fficonfig.h/ffi.h/ffitarget.h) that the library's sources #include. The
#              SHARED path compiles via bare-gcc add_custom_command, which is outside
#              CMake's target dependency graph and so does NOT auto-track configure_file
#              outputs — IMPLICIT_DEPENDS only scans source-local #include at configure
#              time and cannot see headers generated later. Listing them here adds them
#              to each object's DEPENDS so editing the template re-compiles the library.
#              (The STATIC path uses add_library, which tracks configure_file outputs
#              automatically, so GEN_HEADERS is unused there.)
# EXTRA_OBJS: pre-built .o files (e.g. from an OBJECT library via $<TARGET_OBJECTS:...>)
#              to splice into the library archive/link in addition to SOURCES. Used by
#              the musl-unistd integration: musl sources need a different include order
#              (musl src/internal before user/include) than the rest of libc, so they are
#              built by a separate add_library(OBJECT) target (musl_unistd_objs) and its
#              objects are merged into libc.a/libc.so via this parameter. Both the STATIC
#              (add_library STATIC archive) and SHARED (bare-gcc link) paths consume them.
# IMAGE_PATH/IMAGE_ARTIFACT/IMAGE_PARTITION/NO_IMAGE: disk-image manifest registration
#              (reface_cmake.md §6). SHARED auto-defaults to lib/lib<out>.so @ part 2;
#              STATIC has no auto-default (register only with explicit IMAGE_PATH, e.g.
#              libc.a → usr/lib/libc.a). NO_IMAGE opts out of the SHARED default.
# Otherwise: add_library(STATIC), preserve target interface (unity uses target_include_directories)
function(add_user_lib lib_name)
    set(option_args SHARED C NO_IMAGE)
    set(multi_args SOURCES FLAGS SO_LINK_LIBS INCLUDE_DIRS GEN_HEADERS EXTRA_OBJS)
    set(one_args OUTPUT_NAME VERSION_MAP IMAGE_PATH IMAGE_ARTIFACT IMAGE_PARTITION ENTRY)
    cmake_parse_arguments(ARG "${option_args}" "${one_args}" "${multi_args}" ${ARGN})

    # Artifact base name (default = lib_name). The SHARED path's SO_FILE and the
    # disk-image default path both derive from this; OUTPUT_NAME stays untouched
    # for the STATIC if/else below (preserving its original semantics).
    if(ARG_OUTPUT_NAME)
        set(_out_base ${ARG_OUTPUT_NAME})
    else()
        set(_out_base ${lib_name})
    endif()
    # IMAGE_PARTITION default = 2 (root); ESP placement must be explicit.
    if(NOT DEFINED ARG_IMAGE_PARTITION)
        set(ARG_IMAGE_PARTITION 2)
    endif()

    if(ARG_SHARED)
        # Shared library libc.so — add_library(SHARED) is unavailable in this toolchain, use custom command
        # (plan_ld2b3 decision 1 fallback: bare-gcc link line)
        if(ARG_C)
            set(COMPILE_CMD ${CMAKE_C_COMPILER})
            set(DEP_LANG "C")
        else()
            set(COMPILE_CMD ${CMAKE_CXX_COMPILER})
            set(DEP_LANG "CXX")
        endif()
        # FLAGS may be a string (e.g. "-fno-pie -DDYNAMIC=0"), convert to list
        separate_arguments(ARG_FLAGS_LIST UNIX_COMMAND "${ARG_FLAGS}")
        # -fvisibility=hidden: default hidden, only export-marked declarations are exported (consistent with ld.so).
        # When VERSION_MAP is set, .map + verify_libc_exports.sh gates the exports.
        # Libraries like libinput use LIBINPUT_EXPORT (__attribute__((visibility("default")))) markings.
        # -fPIC: required for all .so objects (position-independent code).
        # libc.map owns the public ABI. Do not require every standard function
        # definition to inherit a project-specific visibility annotation.
        set(COMPILE_FLAGS_BASE ${USER_COMPILE_FLAGS} ${USER_BUILD_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/third_party -I${CMAKE_SOURCE_DIR}/include/uapi -I${CMAKE_SOURCE_DIR}/user/include -I${MUSL_GEN_DIR} ${MUSL_INCLUDE_FLAGS} -fPIC ${DRM_INCLUDE_FLAGS} ${ARG_FLAGS_LIST})
        if(ARG_INCLUDE_DIRS)
            foreach(_dir ${ARG_INCLUDE_DIRS})
                list(APPEND COMPILE_FLAGS_BASE -I${_dir})
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
            set(src_obj ${CMAKE_BINARY_DIR}/${lib_name}_${idx}.o)
            add_custom_command(OUTPUT ${src_obj}
                COMMAND ${COMPILE_CMD} ${COMPILE_FLAGS_BASE} -c ${src_full} -o ${src_obj}
                DEPENDS ${src_full} ${ARG_GEN_HEADERS} ${MUSL_GEN_HEADERS}
                IMPLICIT_DEPENDS ${DEP_LANG} ${src_full}
                COMMENT "Compiling ${lib_name}_${idx}.o (SHARED)")
            list(APPEND OBJ_FILES ${src_obj})
            math(EXPR idx "${idx} + 1")
        endforeach()

        # EXTRA_OBJS is spliced into the link below as its own COMMAND argument
        # (see the add_custom_command) — NOT folded into OBJ_FILES here, because
        # $<TARGET_OBJECTS:...> expands to a ;-joined list that would otherwise
        # glue into one list element and confuse the shell.

        set(SO_FILE ${CMAKE_BINARY_DIR}/lib${_out_base}.so)

        # Disk-image manifest: SHARED .so auto-defaults to lib/lib<out>.so @ root.
        # NO_IMAGE opts out; IMAGE_PATH/IMAGE_ARTIFACT/IMAGE_PARTITION override.
        if(NOT ARG_NO_IMAGE)
            set(_img_artifact "lib${_out_base}.so")
            if(ARG_IMAGE_ARTIFACT)
                set(_img_artifact ${ARG_IMAGE_ARTIFACT})
            endif()
            set(_img_dest "lib/lib${_out_base}.so")
            if(ARG_IMAGE_PATH)
                set(_img_dest ${ARG_IMAGE_PATH})
            endif()
            os_image_path(${lib_name} ${_img_artifact} ${_img_dest} PARTITION ${ARG_IMAGE_PARTITION})
        endif()

        # --- Link dependencies (object files + version map if any) ---
        set(SO_LINK_DEPS ${OBJ_FILES})
        if(ARG_VERSION_MAP)
            list(APPEND SO_LINK_DEPS ${CMAKE_SOURCE_DIR}/${ARG_VERSION_MAP})
        endif()
        if(ARG_SO_LINK_LIBS)
            foreach(_so_lib ${ARG_SO_LINK_LIBS})
                list(APPEND SO_LINK_DEPS ${CMAKE_BINARY_DIR}/lib${_so_lib}.so)
            endforeach()
        endif()
        # EXTRA_OBJS carries the object-target dependency too: a $<TARGET_OBJECTS:t>
        # generator expression in DEPENDS makes ninja build target t's objects before
        # this link runs. (The same expression in COMMAND supplies the .o paths.)
        if(ARG_EXTRA_OBJS)
            list(APPEND SO_LINK_DEPS ${ARG_EXTRA_OBJS})
        endif()

        # --- Extra link flags (version script + libc.so dependency) ---
        set(SO_EXTRA_LDFLAGS "")
        if(ARG_ENTRY)
            list(APPEND SO_EXTRA_LDFLAGS "-Wl,-e,${ARG_ENTRY}")
        endif()
        if(ARG_VERSION_MAP)
            list(APPEND SO_EXTRA_LDFLAGS
                 "-Wl,--version-script,${CMAKE_SOURCE_DIR}/${ARG_VERSION_MAP}")
        endif()
        if(ARG_SO_LINK_LIBS)
            list(APPEND SO_EXTRA_LDFLAGS "-L${CMAKE_BINARY_DIR}")
            foreach(_so_lib ${ARG_SO_LINK_LIBS})
                list(APPEND SO_EXTRA_LDFLAGS "-l${_so_lib}")
            endforeach()
        endif()

        # EXTRA_OBJS (e.g. $<TARGET_OBJECTS:musl_unistd_objs>) splices pre-built
        # object files into the link. It is emitted as its own COMMAND argument
        # (kept out of OBJ_FILES) so CMake evaluates the $<TARGET_OBJECTS:...>
        # generator expression at *generate* time and splits its result into one
        # argv entry per object. Folding it into OBJ_FILES via list(APPEND) would
        # collapse the whole expansion into a single list element with embedded
        # semicolons, and the shell would then treat each ';' as a command
        # separator (every object path executed as a command → "Permission
        # denied"). The DEPENDS line below carries the object target dependency.
        set(_extra_objs "")
        if(ARG_EXTRA_OBJS)
            set(_extra_objs ${ARG_EXTRA_OBJS})
        endif()

        add_custom_command(OUTPUT ${SO_FILE}
            COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -nostdlib -nodefaultlibs
                    ${USER_PERF_LINK_FLAGS}
                    -Wl,--hash-style=gnu
                    -Wl,-soname,lib${ARG_OUTPUT_NAME}.so
                    ${SO_EXTRA_LDFLAGS}
                    -o ${SO_FILE} ${OBJ_FILES} ${_extra_objs}
            DEPENDS ${SO_LINK_DEPS}
            COMMAND_EXPAND_LISTS
            COMMENT "Linking ${lib_name}.so")

        # Post-link verification (libc-specific, only when VERSION_MAP is provided)
        # NOTE: APPEND COMMAND cannot specify DEPENDS — that would create a
        # self-referencing rule (OUTPUT depends on OUTPUT). The dependency on
        # SO_FILE is already implicit from the OUTPUT-matching APPEND mechanism.
        if(ARG_VERSION_MAP)
            add_custom_command(OUTPUT ${SO_FILE}
                COMMAND bash ${CMAKE_SOURCE_DIR}/build_script/cmake/verify_so_init_array.sh ${SO_FILE}
                COMMAND bash ${CMAKE_SOURCE_DIR}/build_script/cmake/verify_libc_exports.sh ${SO_FILE} ${CMAKE_SOURCE_DIR}/${ARG_VERSION_MAP}
                APPEND COMMAND)
        endif()

        add_custom_target(${lib_name} ALL DEPENDS ${SO_FILE})
        # EXTRA_OBJS carries an object-target dependency too: a $<TARGET_OBJECTS:t>
        # generator expression in DEPENDS (SO_LINK_DEPS above) makes ninja build
        # target t's objects before this link runs.
    else()
        # Static library — add_library(STATIC), preserve target interface.
        # EXTRA_OBJS (e.g. $<TARGET_OBJECTS:musl_unistd_objs>) are passed as additional
        # sources; CMake archives them into the .a alongside the compiled SOURCES.
        add_library(${lib_name} STATIC ${ARG_SOURCES} ${ARG_EXTRA_OBJS})

        # Include paths: project root + third_party + user/include (our stdio/
        # string/time/unistd win over musl's) + musl_gen (generated <bits/
        # alltypes.h> — 128-byte sigset_t / struct sigaction that our libc's
        # signal.cc/io_multiplex.cc use) + musl headers (pthread.h/signal.h/
        # sched.h — ours deleted — and arch bits). user/include FIRST so our
        # libc headers win; musl_gen + musl appended after so pthread/signal
        # resolve. Added unconditionally because our stdio.h includes <pthread.h>,
        # which now lives only in musl/include — every static user lib (libc,
        # libm, libinput, ...) needs it.
        target_include_directories(${lib_name} PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/third_party
            ${CMAKE_SOURCE_DIR}/user/include
            ${MUSL_GEN_DIR}
            ${CMAKE_SOURCE_DIR}/third_party/musl/include
            ${CMAKE_SOURCE_DIR}/third_party/musl/arch/x86_64
            ${CMAKE_SOURCE_DIR}/third_party/musl/arch/generic
        )
        # UAPI contract headers (include/uapi → #include "xos/*.h") via os_uapi,
        # replacing the prior root-scope include_directories(include/uapi).
        target_link_libraries(${lib_name} PRIVATE os_uapi)

        if(ARG_FLAGS)
            separate_arguments(ARG_FLAGS_LIST UNIX_COMMAND "${ARG_FLAGS}")
            target_compile_options(${lib_name} PRIVATE ${USER_COMPILE_FLAGS} ${ARG_FLAGS_LIST})
        else()
            target_compile_options(${lib_name} PRIVATE ${USER_COMPILE_FLAGS})
        endif()

        if(ARG_OUTPUT_NAME)
            set_target_properties(${lib_name} PROPERTIES
                ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                OUTPUT_NAME "${ARG_OUTPUT_NAME}"
            )
        else()
            set_target_properties(${lib_name} PROPERTIES
                ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
            )
        endif()

        user_assert_no_sse_disable(${lib_name})

        # Generated musl headers (bits/alltypes.h / bits/syscall.h) must exist
        # before any source compiles — every static user lib pulls musl headers
        # (stdio.h -> <pthread.h> -> <bits/alltypes.h>) via the include dirs above.
        add_dependencies(${lib_name} musl_headers)

        # Disk-image manifest: STATIC has NO auto-default (most static libs aren't
        # shipped). Register only with an explicit IMAGE_PATH (e.g. libc.a →
        # usr/lib/libc.a). IMAGE_ARTIFACT defaults to lib<out>.a. NO_IMAGE is a
        # no-op here but accepted for call-site symmetry with SHARED.
        if(ARG_IMAGE_PATH AND NOT ARG_NO_IMAGE)
            set(_img_artifact "lib${_out_base}.a")
            if(ARG_IMAGE_ARTIFACT)
                set(_img_artifact ${ARG_IMAGE_ARTIFACT})
            endif()
            os_image_path(${lib_name} ${_img_artifact} ${ARG_IMAGE_PATH} PARTITION ${ARG_IMAGE_PARTITION})
        endif()
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
    # project-wide uapi, user/include, and musl headers — our stdio.h includes
    # <pthread.h>, which after the musl switch lives only in musl/include).
    # Library-specific headers go via INCLUDE_DIRS so the lib controls its own
    # resolution order.
    target_include_directories(${lib_name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/include/uapi
        ${CMAKE_SOURCE_DIR}/user/include
        ${MUSL_GEN_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/musl/include
        ${CMAKE_SOURCE_DIR}/third_party/musl/arch/x86_64
        ${CMAKE_SOURCE_DIR}/third_party/musl/arch/generic
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
    target_compile_options(${lib_name} PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -fno-pie
        ${ARG_FLAGS_LIST} ${THIRD_PARTY_OPT_FLAGS})

    set_target_properties(${lib_name} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    user_assert_no_sse_disable(${lib_name})

    # Generated musl headers must exist before drm sources compile (xf86drm*.c
    # include <stdint.h> -> musl's stdint.h -> <bits/alltypes.h>).
    add_dependencies(${lib_name} musl_headers)
endfunction()

# musl_generate_headers: produce musl's two build-time-generated headers
# (bits/alltypes.h from arch/x86_64/bits/alltypes.h.in + include/alltypes.h.in
# via tools/mkalltypes.sed; bits/syscall.h from arch/x86_64/bits/syscall.h.in
# via __NR_→SYS_ sed) into ${CMAKE_BINARY_DIR}/musl_gen. musl ships only the
# .in templates; the generated headers are required for any compilation that
# pulls musl's <pthread.h>/<signal.h>/internal headers (they #include
# <bits/alltypes.h>, which only exists after generation). No musl ./configure
# is needed — these two sed steps are the entire generation musl requires for
# the headers we use.
#
# Exports:
#   MUSL_GEN_INCLUDE_DIR — directory to put FIRST on include paths so
#     <bits/alltypes.h> / <bits/syscall.h> resolve to the generated copies
#     (they live under musl_gen/bits/, shadowing nothing — the arch .in files
#     have different names). Non-generated <bits/signal.h> etc. still resolve
#     from third_party/musl/arch/x86_64.
#   musl_headers target  — depends all consumers add_dependencies on, so the
#     headers exist before any musl source or consumer compiles.
function(musl_generate_headers)
    set(MUSL_SRC ${CMAKE_SOURCE_DIR}/third_party/musl)
    set(MUSL_GEN_INCLUDE_DIR ${CMAKE_BINARY_DIR}/musl_gen PARENT_SCOPE)
    set(_gendir ${CMAKE_BINARY_DIR}/musl_gen/bits)
    file(MAKE_DIRECTORY ${_gendir})

    add_custom_command(
        OUTPUT ${_gendir}/alltypes.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gendir}
        COMMAND sed -f ${MUSL_SRC}/tools/mkalltypes.sed
                ${MUSL_SRC}/arch/x86_64/bits/alltypes.h.in
                ${MUSL_SRC}/include/alltypes.h.in
                > ${_gendir}/alltypes.h
        DEPENDS ${MUSL_SRC}/arch/x86_64/bits/alltypes.h.in
                ${MUSL_SRC}/include/alltypes.h.in
                ${MUSL_SRC}/tools/mkalltypes.sed
        COMMENT "Generating musl bits/alltypes.h")

    add_custom_command(
        OUTPUT ${_gendir}/syscall.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gendir}
        COMMAND cp ${MUSL_SRC}/arch/x86_64/bits/syscall.h.in ${_gendir}/syscall.h
        COMMAND sed -n -e s/__NR_/SYS_/p
                < ${MUSL_SRC}/arch/x86_64/bits/syscall.h.in
                >> ${_gendir}/syscall.h
        DEPENDS ${MUSL_SRC}/arch/x86_64/bits/syscall.h.in
        COMMENT "Generating musl bits/syscall.h")

    # version.h — consumed by src/internal/version.c (`const char __libc_version[]
    # = VERSION;`), which the 1.2.x loader references for its ldd banner
    # (dynlink.c:1895). musl's Makefile generates obj/src/internal/version.h from
    # `sh tools/version.sh` (which, with no .git, just `cat VERSION`). We mirror
    # that into musl_gen (top level, NOT bits/) so src/internal/version.c's
    # #include "version.h" resolves to it via MUSL_GEN_INCLUDE_DIR. v1.1.19's
    # loader did not reference __libc_version, so this was not generated before.
    # Done at configure time (file READ/WRITE) rather than as a build-time custom
    # command: VERSION is a checked-in static file, and shell $(cat) inside a
    # CMake custom command collides with ninja's $var syntax.
    file(READ ${MUSL_SRC}/VERSION _musl_version)
    string(STRIP "${_musl_version}" _musl_version)
    file(WRITE ${CMAKE_BINARY_DIR}/musl_gen/version.h
         "#define VERSION \"${_musl_version}\"\n")
    file(WRITE ${_gendir}/posix.h
         "#define _POSIX_V6_LP64_OFF64 1\n#define _POSIX_V7_LP64_OFF64 1\n")

    add_custom_target(musl_headers ALL
        DEPENDS ${_gendir}/alltypes.h ${_gendir}/syscall.h)
endfunction()

# add_musl_lib: musl upstream source compiled into an OBJECT library (relaxed
# warnings, musl-internal include paths). OBJECT (not STATIC) so its objects
# can be merged directly into libc.a and libc.so via $<TARGET_OBJECTS:...>
# (see add_user_lib EXTRA_OBJS) — musl's design puts pthread in libc, so
# musl_pthread has no standalone .a/.so product. Mirrors add_drm_lib's
# third-party posture (no WARN_FLAGS, own -I list) but fixes the include order
# for musl: musl's own headers (src/internal, arch/x86_64, arch/generic,
# include) MUST resolve before the GCC freestanding -isystem dir shipped in
# FREESTANDING_FLAGS, so we prepend them via target_include_directories(BEFORE)
# rather than APPEND. The generated-headers dir (MUSL_GEN_INCLUDE_DIR, from
# musl_generate_headers) is prepended FIRST so <bits/alltypes.h>/<bits/syscall.h>
# resolve to the generated copies.
# Usage: add_musl_lib(name SOURCES ... [INCLUDE_DIRS ...] [FLAGS ...])
function(add_musl_lib lib_name)
    set(option_args "")
    set(multi_args SOURCES INCLUDE_DIRS FLAGS)
    cmake_parse_arguments(ARG "${option_args}" "" "${multi_args}" ${ARGN})

    add_library(${lib_name} OBJECT ${ARG_SOURCES})

    # musl-internal + arch headers first (BEFORE so they win over the
    # FREESTANDING_FLAGS -isystem GCC dir), then the project-wide baselines.
    # MUSL_GEN_INCLUDE_DIR first so generated bits/alltypes.h & bits/syscall.h
    # win over the arch .in templates. src/include precedes src/internal per
    # musl's own Makefile order (-Isrc/include -Isrc/internal): since musl
    # 1.2.x, src/internal/syscall.h starts with #include <features.h>, and the
    # internal macros hidden/weak/weak_alias are defined ONLY in
    # src/include/features.h (v1.1.19 defined weak_alias in libc.h and had no
    # hidden keyword at all). Without src/include on the path, <features.h>
    # falls through to user/include/features.h and every musl source fails with
    # "unknown type name 'hidden'".
    target_include_directories(${lib_name} BEFORE PRIVATE
        ${MUSL_GEN_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/musl/arch/x86_64
        ${CMAKE_SOURCE_DIR}/third_party/musl/arch/generic
        ${CMAKE_SOURCE_DIR}/third_party/musl/src/include
        ${CMAKE_SOURCE_DIR}/third_party/musl/src/internal
        ${CMAKE_SOURCE_DIR}/third_party/musl/include
        ${ARG_INCLUDE_DIRS}
    )
    target_include_directories(${lib_name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include/uapi
        ${CMAKE_SOURCE_DIR}/user/include
    )
    # Generated headers must exist before any musl source compiles.
    add_dependencies(${lib_name} musl_headers)

    # Relaxed flags: -m64 + freestanding (already carries -fno-stack-protector,
    # -nostdinc + -isystem GCC dir) + -fPIC. The same objects merge into BOTH
    # libc.a (static, -no-pie ELFs) and libc.so (shared). -fPIC is required for
    # the .so link (a non-PIC object referencing a global like __libc yields
    # R_X86_64_32S, rejected when making a shared object); -fPIC objects also
    # link fine into a static -no-pie ELF, so one compile serves both consumers.
    # NO WARN_FLAGS (musl upstream is not subject to our -Werror gate).
    # -Wno-everything silences musl's own warnings under our freestanding setup.
    # -D_XOPEN_SOURCE=700 mirrors musl's own Makefile (CFLAGS_ALL += -D_XOPEN_SOURCE=700)
    # and the loader target in musl_rules.cmake. It is load-bearing: without it,
    # musl <features.h>'s default branch fires (it enables _BSD_SOURCE only when
    # _XOPEN_SOURCE is undefined) and sets _BSD_SOURCE=1. That in turn makes
    # <unistd.h>'s `#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)` block
    # (which declares `long syscall(long, ...)`) active — and since
    # src/internal/syscall.h #defines `syscall(...)` as a macro, that prototype
    # is macro-expanded into a garbage `__syscall_ret(...)` declaration that
    # conflicts with the real one (musl 1.2.x; v1.1.19 did not ship
    # src/include/unistd.h so the collision did not arise). With _XOPEN_SOURCE=700
    # set, _BSD_SOURCE stays undefined and the `long syscall` prototype is skipped.
    separate_arguments(ARG_FLAGS_LIST UNIX_COMMAND "${ARG_FLAGS}")
    target_compile_options(${lib_name} PRIVATE
        -m64 ${FREESTANDING_FLAGS} -fPIC -Wno-everything -D_XOPEN_SOURCE=700
        ${ARG_FLAGS_LIST} ${THIRD_PARTY_OPT_FLAGS})
    # -Wno-everything does NOT silence -Wvisibility in clang (visibility is a
    # hard warning outside the -everything group, like -Wempty-body below).
    # musl's <termios.h> declares tcgetwinsize/tcsetwinsize taking `struct
    # winsize *` after setting __NEED_struct_winsize; if the resolved
    # bits/alltypes.h does not emit the struct definition (a musl-version /
    # generated-header mismatch), clang warns the declaration "will not be
    # visible outside of this function" on every src/unistd/tc{get,set}pgrp.c
    # compile. musl is third-party upstream not under our -Werror gate, so
    # silence it here across all musl modules — same rationale as -Wno-everything.
    target_compile_options(${lib_name} PRIVATE -Wno-visibility)
    # musl's bare .s sources (src/signal/x86_64/restore.s, src/thread/x86_64/*.s,
    # src/internal/x86_64/syscall.s) carry no .note.GNU-stack, so each linked
    # object without the note trips `ld: warning: ... missing .note.GNU-stack
    # section implies executable stack` (one warning per ELF link, attributed to
    # whichever note-less .o the linker hits first — here restore.s.obj).
    # -Wa,--noexecstack makes the assembler inject an empty .note.GNU-stack,
    # matching the crti.s/crtn.s handling in musl_rules.cmake. Harmless on the C
    # sources: clang already emits the note, so this only adds it to the .s ones.
    target_compile_options(${lib_name} PRIVATE -Wa,--noexecstack)

    user_assert_no_sse_disable(${lib_name})
endfunction()

# musl crt objects (Scrt1/crt1/crti/crtn) are produced by the musl subproject
# target `musl_libc` under ${MUSL_LIB_DIR} (build_script/cmake/musl_rules.cmake).
# Static main ELFs (add_user_elf) link crt1.o + crti.o + crtn.o; dynamic main
# ELFs (add_user_dyn_elf) link the PIC Scrt1.o + crti.o + crtn.o. The hand-
# written user/lib/crt0.S is retired (ldso.md Phase 1/2). Callers depend on
# musl_libc so these .o exist before the link.

# add_user_elf: userspace ELF (compile → objcopy → ld)
# Usage: add_user_elf(name [C] SOURCES source1 ... [LINK_LIBS lib1 ...] [DEFS def1 ...]
#                     [INCLUDE_DIRS ...] [IMAGE_PATH dest] [IMAGE_ARTIFACT name]
#                     [IMAGE_PARTITION <1|2>] [NO_IMAGE])
# IMAGE_PATH: explicit disk-image destination (no auto-default — most static ELFs
#   are non-test and shipped individually). IMAGE_ARTIFACT defaults to <name>.elf.
#   IMAGE_PARTITION defaults to 2. NO_IMAGE is a no-op (no default to suppress).
function(add_user_elf elf_name)
    cmake_parse_arguments(ARG "C;NO_IMAGE" "IMAGE_PATH;IMAGE_ARTIFACT;IMAGE_PARTITION" "SOURCES;LINK_LIBS;DEFS;INCLUDE_DIRS" ${ARGN})

    set(ELF_DIR ${CMAKE_BINARY_DIR})
    set(ELF_FILE ${ELF_DIR}/${elf_name}.elf)

    # Compiler selection
    if(ARG_C)
        set(COMPILE_CMD ${CMAKE_C_COMPILER})
    else()
        set(COMPILE_CMD ${CMAKE_CXX_COMPILER})
    endif()
    set(COMPILE_FLAGS ${USER_COMPILE_FLAGS} ${USER_BUILD_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/third_party -I${CMAKE_SOURCE_DIR}/include/uapi -I${CMAKE_SOURCE_DIR}/user/include -I${MUSL_GEN_DIR} ${MUSL_INCLUDE_FLAGS} ${DRM_INCLUDE_FLAGS} -I${CMAKE_SOURCE_DIR}/third_party/Unity/src)

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
        # Propagate INTERFACE_COMPILE_DEFINITIONS from linked targets (e.g. unity's
        # UNITY_EXCLUDE_MATH_H) so consumers need not re-declare.
        _collect_iface_compile_definitions(_iface_defs ${ARG_LINK_LIBS})
        list(APPEND COMPILE_FLAGS ${_iface_defs})
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
            DEPENDS ${src_full} ${MUSL_GEN_HEADERS}
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

    # Step 2: ld — musl crt1.o (static _start) + crti.o/crtn.o (.init/.fini
    # brackets) must bracket the object files. crt1.o is first (provides
    # _start → _start_c → __libc_start_main).
    set(MUSL_CRT  ${MUSL_LIB_DIR}/crt1.o ${MUSL_LIB_DIR}/crti.o)
    set(MUSL_CRTN ${MUSL_LIB_DIR}/crtn.o)
    set(LD_DEPS ${MUSL_CRT} ${OBJ_FILES} ${MUSL_CRTN})
    set(LD_ARGS ${MUSL_CRT} ${OBJ_FILES} ${MUSL_CRTN})

    if(ARG_LINK_LIBS)
        # Use --start-group/--end-group to handle circular dependencies within
        # and between static libraries (e.g. libinput internal .o -> .o refs).
        list(APPEND LD_ARGS "--start-group")
        foreach(lib ${ARG_LINK_LIBS})
            list(APPEND LD_DEPS ${CMAKE_BINARY_DIR}/lib${lib}.a)
            list(APPEND LD_ARGS ${CMAKE_BINARY_DIR}/lib${lib}.a)
        endforeach()
        list(APPEND LD_ARGS "--end-group")
    endif()

    add_custom_command(
        OUTPUT ${ELF_FILE}
        COMMAND ld -m elf_x86_64 ${USER_PERF_LD_FLAGS} -T ${CMAKE_SOURCE_DIR}/build_script/user_linker.ld ${LD_ARGS} -o ${ELF_FILE}
        DEPENDS ${LD_DEPS} ${CMAKE_SOURCE_DIR}/build_script/user_linker.ld
        COMMENT "Linking ${elf_name}.elf"
    )

    add_custom_target(${elf_name}_elf ALL DEPENDS ${ELF_FILE})

    # musl_libc produces crt1.o/crti.o/crtn.o
    add_dependencies(${elf_name}_elf musl_libc)
    if(ARG_LINK_LIBS)
        add_dependencies(${elf_name}_elf ${ARG_LINK_LIBS})
    endif()

    # Disk-image manifest: no auto-default — register only with explicit IMAGE_PATH.
    if(ARG_IMAGE_PATH AND NOT ARG_NO_IMAGE)
        set(_img_artifact "${elf_name}.elf")
        if(ARG_IMAGE_ARTIFACT)
            set(_img_artifact ${ARG_IMAGE_ARTIFACT})
        endif()
        if(NOT DEFINED ARG_IMAGE_PARTITION)
            set(ARG_IMAGE_PARTITION 2)
        endif()
        os_image_path(${elf_name} ${_img_artifact} ${ARG_IMAGE_PATH} PARTITION ${ARG_IMAGE_PARTITION})
    endif()
endfunction()

# add_user_dyn_elf: dynamic main ELF, linked by gcc driver
# ldso.md: musl Scrt1.o (PIC _start) + crti.o/crtn.o bracket the objects; libc.so
# (the fused musl-loader + hand-written libc) is linked via -L/-l so the main
# ELF records DT_NEEDED libc.so, which the fused interp (=/lib/ld-musl-x86_64.so.1
# = /lib/libc.so) satisfies at runtime. PT_INTERP → /lib/ld-musl-x86_64.so.1.
# Supports both C and C++ sources: pass C flag for C, omit for C++ (like add_user_elf)
# GEN_HEADERS: generated headers (configure_file outputs in ${CMAKE_BINARY_DIR}) that
#              the ELF's sources #include. Same rationale as add_user_lib's GEN_HEADERS:
#              bare-gcc add_custom_command doesn't auto-track configure_file outputs, so
#              listing them forces a re-compile when the template changes.
function(add_user_dyn_elf name)
    cmake_parse_arguments(ARG "C;NO_IMAGE;EXCLUDE_FROM_ALL" "IMAGE_PATH;IMAGE_ARTIFACT;IMAGE_PARTITION" "SOURCES;LINK_LIBS;STATIC_LIBS;DEFS;INCLUDE_DIRS;GEN_HEADERS;FLAGS" ${ARGN})
    set(ELF_FILE ${CMAKE_BINARY_DIR}/${name}.elf)
    if(ARG_C)
        set(COMPILE_CMD ${CMAKE_C_COMPILER})
    else()
        set(COMPILE_CMD ${CMAKE_CXX_COMPILER})
    endif()
    set(COMPILE_FLAGS ${USER_COMPILE_FLAGS} ${USER_BUILD_FLAGS} -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/third_party -I${CMAKE_SOURCE_DIR}/include/uapi -I${CMAKE_SOURCE_DIR}/user/include -I${MUSL_GEN_DIR} ${MUSL_INCLUDE_FLAGS} ${DRM_INCLUDE_FLAGS} -I${CMAKE_SOURCE_DIR}/third_party/Unity/src ${ARG_FLAGS})

    # Extra include directories
    if(ARG_INCLUDE_DIRS)
        foreach(inc ${ARG_INCLUDE_DIRS})
            list(APPEND COMPILE_FLAGS -I${inc})
        endforeach()
    endif()

    # Propagate INTERFACE_INCLUDE_DIRECTORIES from linked library targets
    # (same logic as add_user_elf — see comment there). Both LINK_LIBS (.so) and
    # STATIC_LIBS (.a) carry usage requirements (e.g. unity's INTERFACE include +
    # PUBLIC compile definitions), so merge them for compile-time propagation.
    set(_all_link_libs ${ARG_LINK_LIBS} ${ARG_STATIC_LIBS})
    if(_all_link_libs)
        foreach(lib ${_all_link_libs})
            if(TARGET ${lib})
                get_target_property(_iface_includes ${lib} INTERFACE_INCLUDE_DIRECTORIES)
                if(_iface_includes)
                    foreach(inc ${_iface_includes})
                        list(APPEND COMPILE_FLAGS -I${inc})
                    endforeach()
                endif()
            endif()
        endforeach()
        # Propagate INTERFACE_COMPILE_DEFINITIONS from linked targets (e.g. unity's
        # UNITY_EXCLUDE_MATH_H) so consumers need not re-declare.
        _collect_iface_compile_definitions(_iface_defs ${_all_link_libs})
        list(APPEND COMPILE_FLAGS ${_iface_defs})
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
            COMMAND ${COMPILE_CMD} ${COMPILE_FLAGS} -MMD -MF ${src_obj}.d
                    -c ${src_full} -o ${src_obj}
            DEPENDS ${src_full} ${ARG_GEN_HEADERS} ${MUSL_GEN_HEADERS}
            IMPLICIT_DEPENDS ${DEP_LANG} ${src_full}
            DEPFILE ${src_obj}.d)
        list(APPEND OBJ_FILES ${src_obj})
        math(EXPR idx "${idx} + 1")
    endforeach()

    # musl Scrt1.o (PIC _start) first, crti.o brackets the .init, crtn.o closes
    # it after the objects. libc.so linked via -L/-l (records DT_NEEDED libc.so).
    set(MUSL_CRT  ${MUSL_LIB_DIR}/Scrt1.o ${MUSL_LIB_DIR}/crti.o)
    set(MUSL_CRTN ${MUSL_LIB_DIR}/crtn.o)
    set(LD_ARGS ${MUSL_CRT} ${OBJ_FILES} ${MUSL_CRTN})
    set(SO_DEPS ${MUSL_CRT} ${MUSL_CRTN})
    # LINK_LIBS → lib<lib>.so (dynamic, records DT_NEEDED; full path avoids -lc
    # selecting libc.a). STATIC_LIBS → lib<lib>.a (compile-time link, e.g. unity —
    # a true STATIC library with no .so variant). Separating the two avoids the
    # creation-order ambiguity of detecting .so variants by target scan.
    foreach(lib ${ARG_LINK_LIBS})
        set(_lib_path ${CMAKE_BINARY_DIR}/lib${lib}.so)
        list(APPEND LD_ARGS ${_lib_path})
        list(APPEND SO_DEPS ${_lib_path})
    endforeach()
    foreach(lib ${ARG_STATIC_LIBS})
        set(_lib_path ${CMAKE_BINARY_DIR}/lib${lib}.a)
        list(APPEND LD_ARGS ${_lib_path})
        list(APPEND SO_DEPS ${_lib_path})
    endforeach()
    add_custom_command(OUTPUT ${ELF_FILE}
        COMMAND ${CMAKE_C_COMPILER} -fno-pie -no-pie
                ${USER_PERF_LINK_FLAGS}
                -Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1
                -Wl,--hash-style=gnu
                -Wl,--no-as-needed
                -Wl,--allow-shlib-undefined
                -nostdlib -nodefaultlibs
                -o ${ELF_FILE} ${LD_ARGS}
        DEPENDS ${OBJ_FILES} ${SO_DEPS}
        COMMENT "Linking dynamic ${name}.elf")
    if(ARG_EXCLUDE_FROM_ALL)
        add_custom_target(${name}_dyn_elf DEPENDS ${ELF_FILE})
    else()
        add_custom_target(${name}_dyn_elf ALL DEPENDS ${ELF_FILE})
    endif()
    add_dependencies(${name}_dyn_elf musl_libc)
    # Build ordering for LINK_LIBS (.so variant targets, named <lib>_so or
    # lib<lib>_so) and STATIC_LIBS (the lib target itself). CMake archives are not
    # path-resolvable by Ninja from a bare DEPENDS file list, so declare target deps.
    foreach(lib ${ARG_LINK_LIBS})
        foreach(_cand ${lib}_so lib${lib}_so)
            if(TARGET ${_cand})
                add_dependencies(${name}_dyn_elf ${_cand})
            endif()
        endforeach()
    endforeach()
    foreach(lib ${ARG_STATIC_LIBS})
        if(TARGET ${lib})
            add_dependencies(${name}_dyn_elf ${lib})
        endif()
    endforeach()

    # Disk-image manifest (reface_cmake.md §6). Auto-default test/<name>.elf @ root —
    # the majority of add_user_dyn_elf callers are test ELFs (user/test/CMakeLists.txt
    # + a few in user/CMakeLists.txt like test_fpu/ld_test_*), all shipped under /test/.
    # Non-test dyn ELFs (hello_dyn/terminal/hello_drm_dyn/modetest_dyn) must pass an
    # explicit IMAGE_PATH or NO_IMAGE to avoid being mis-shipped to /test/.
    if(NOT ARG_NO_IMAGE)
        set(_img_artifact "${name}.elf")
        if(ARG_IMAGE_ARTIFACT)
            set(_img_artifact ${ARG_IMAGE_ARTIFACT})
        endif()
        set(_img_dest "test/${name}.elf")
        if(ARG_IMAGE_PATH)
            set(_img_dest ${ARG_IMAGE_PATH})
        endif()
        if(NOT DEFINED ARG_IMAGE_PARTITION)
            set(ARG_IMAGE_PARTITION 2)
        endif()
        os_image_path(${name} ${_img_artifact} ${_img_dest} PARTITION ${ARG_IMAGE_PARTITION})
    endif()
endfunction()
