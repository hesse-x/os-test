# sbase build rules — vendored sbase (third_party/sbase, suckless base tools).
#
# sbase is a portable UNIX base-tool set (busybox-class, suckless). Upstream
# Makefile builds libutf.a + libutil.a then links every tool.c against both.
# We mirror that with two pieces:
#
#   1. sbase_lib — STATIC lib (libutf/* + libutil/*) via add_drm_lib, the
#      relaxed-warn third_party static-lib helper (NO WARN_FLAGS/-Werror gate;
#      sbase upstream is not subject to our warning gate).
#   2. one user ELF per tool  →  /usr/bin/<tool>  (dynamically linked to
#      libc.so, like terminal/udevd; sbase_lib stays a compile-time static
#      archive since it is our own vendored helper, not a shipped runtime .so).
#
# Tool source set is globbed: every third_party/sbase/*.c that has its own
# main(). add_user_elf is NOT used for the tools because it hard-applies our
# WARN_FLAGS (-Werror) gate, which sbase upstream code does not pass (many
# -Wsign-compare / -Wunused-parameter). Instead each tool is built with a
# bare-gcc custom command mirroring add_user_elf's compile + musl-crt link,
# but with -w (warnings disabled) — same posture as the sbase_lib archive.
#
# Tools that fail to LINK (real missing libc syscall, not a warning) are
# excluded via SBASE_LINK_EXCLUDE — see the comment there. Compile failures
# are not expected once -w is on; if one appears, add the tool there too.
#
# Upstream CPPFLAGS (config.mk + Makefile): -DPREFIX -D_DEFAULT_SOURCE
# -D_NETBSD_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64.
# PREFIX is /usr (FHS install root on the image, matches /usr/bin destination).

set(SBASE_DIR ${CMAKE_SOURCE_DIR}/third_party/sbase)

# ---- sbase_lib: libutf + libutil (static, relaxed warnings) ----
file(GLOB SBASE_LIBUTF_SRC  ${SBASE_DIR}/libutf/*.c)
file(GLOB SBASE_LIBUTIL_SRC ${SBASE_DIR}/libutil/*.c)

set(SBASE_DEFS
    -DPREFIX=\"/usr\"
    -D_DEFAULT_SOURCE
    -D_NETBSD_SOURCE
    -D_BSD_SOURCE
    -D_XOPEN_SOURCE=700
    -D_FILE_OFFSET_BITS=64)

add_drm_lib(sbase_lib
    SOURCES ${SBASE_LIBUTF_SRC} ${SBASE_LIBUTIL_SRC}
    INCLUDE_DIRS ${SBASE_DIR} ${SBASE_DIR}/libutf ${SBASE_DIR}/libutil
    FLAGS "${SBASE_DEFS}")

# Expose sbase's own headers (utf.h/util.h/arg.h/fs.h/text.h/crypt.h/queue.h +
# sha*.h) to tool compiles via INTERFACE include dirs (consumers linking
# sbase_lib inherit them).
target_include_directories(sbase_lib INTERFACE ${SBASE_DIR})

# ---- tool source set (glob root *.c, keep only files with main()) ----
file(GLOB SBASE_TOOL_CANDIDATES ${SBASE_DIR}/*.c)

# Tools we deliberately do not build. Each entry is a root .c basename.
#   bc.c          — absent; built from bc.y via yacc (not run here).
#   getconf.c     — needs generated getconf.h (scripts/getconf.sh emits an
#                   #ifdef sysconf/pathconf/confstr table via host cpp probe);
#                   skipped to keep the build hermetic.
# Link-excluded tools — reference libc symbols our libc.a does not yet export
# (syscalls not wired or stdlib bits not ported). Verified absent via
# `nm libc.a`. Drop a tool here when its link fails on a real missing symbol;
# re-enable by removing the entry once libc gains the symbol.
#   chroot        — chroot(2)
#   mkfifo        — mkfifo(3)
#   mktemp        — mkdtemp(3)
#   cron          — daemon(3)
#   touch         — futimens(3)
#   ed, dc        — system(3)
#   du            — tsearch(3) (glibc search-tree)
#   tftp          — getaddrinfo/freeaddrinfo/gai_strerror (network resolver)
set(SBASE_COMPILE_EXCLUDE
    bc.c
    getconf.c
    chroot.c
    mkfifo.c
    mktemp.c
    cron.c
    touch.c
    ed.c
    dc.c
    du.c
    tftp.c)

# sbase tool → /usr/bin name remap: upstream's `xinstall` binary installs as
# `install`, and `make/make` is a subdir tool (not globbed here). Map the
# handful whose .c basename differs from the on-disk command name.
set(SBASE_NAME_xinstall install)

function(_sbase_tool_name src out_var)
    get_filename_component(_base "${src}" NAME_WE)
    set(_name ${_base})
    set(_mapped "${SBASE_NAME_${_base}}")
    if(_mapped)
        set(_name ${_mapped})
    endif()
    set(${out_var} "${_name}" PARENT_SCOPE)
endfunction()

function(_sbase_is_tool src out_var)
    get_filename_component(_base "${src}" NAME)
    list(FIND SBASE_COMPILE_EXCLUDE "${_base}" _idx)
    if(NOT _idx EQUAL -1)
        set(${out_var} 0 PARENT_SCOPE)
        return()
    endif()
    # Keep only files defining main() (drops any root-level lib-like .c).
    execute_process(COMMAND grep -Eq "^main\\(" "${src}" RESULT_VARIABLE _g)
    if(NOT _g EQUAL 0)
        set(${out_var} 0 PARENT_SCOPE)
        return()
    endif()
    set(${out_var} 1 PARENT_SCOPE)
endfunction()

set(SBASE_TOOLS "")
foreach(_src ${SBASE_TOOL_CANDIDATES})
    _sbase_is_tool("${_src}" _keep)
    if(_keep)
        list(APPEND SBASE_TOOLS "${_src}")
    endif()
endforeach()

# ---- per-tool ELF build (bare-gcc, -w, dynamic libc.so link) ----
# Mirrors add_user_dyn_elf's compile + link, minus WARN_FLAGS, plus -w. Each
# tool is a -fno-pie -no-pie dynamic executable: musl Scrt1.o (PIC _start) +
# crti.o/crtn.o brackets, libsbase_lib.a linked at compile time, libc.so
# pulled in dynamically (DT_NEEDED libc.so) via the musl dynamic loader
# /lib/ld-musl-x86_64.so.1. sbase_lib is non-PIC (add_drm_lib builds with
# -fno-pie, no -fPIC), which is fine for a -no-pie executable. Each tool ships
# to usr/bin/<tool> (manifest-driven).
set(SBASE_COMPILE_FLAGS
    -m64
    ${USER_FREESTANDING_FLAGS}
    -fno-pie
    -w
    -I${CMAKE_SOURCE_DIR}
    -I${CMAKE_SOURCE_DIR}/third_party
    -I${CMAKE_SOURCE_DIR}/include/uapi
    -I${CMAKE_SOURCE_DIR}/user/include
    -I${MUSL_GEN_DIR}
    ${MUSL_INCLUDE_FLAGS}
    -I${SBASE_DIR}
    ${SBASE_DEFS}
    ${THIRD_PARTY_OPT_FLAGS})

# Scrt1.o (PIC _start) for dynamic executables; crti.o/crtn.o bracket .init/.fini.
set(SBASE_MUSL_CRT  ${MUSL_LIB_DIR}/Scrt1.o ${MUSL_LIB_DIR}/crti.o)
set(SBASE_MUSL_CRTN ${MUSL_LIB_DIR}/crtn.o)
set(SBASE_LIBSBASE  ${CMAKE_BINARY_DIR}/libsbase_lib.a)
set(SBASE_LIBC_SO   ${CMAKE_BINARY_DIR}/libc.so)

foreach(_src ${SBASE_TOOLS})
    _sbase_tool_name("${_src}" _tool)
    set(_obj   ${CMAKE_BINARY_DIR}/sbase_${_tool}.o)
    set(_elf   ${CMAKE_BINARY_DIR}/sbase_${_tool}.elf)
    set(_tgt   sbase_${_tool}_elf)

    add_custom_command(
        OUTPUT ${_obj}
        COMMAND ${CMAKE_C_COMPILER} ${SBASE_COMPILE_FLAGS} -c ${_src} -o ${_obj}
        DEPENDS ${_src} ${MUSL_GEN_HEADERS}
        IMPLICIT_DEPENDS C ${_src}
        COMMENT "Compiling sbase/${_tool}.o")
    add_custom_command(
        OUTPUT ${_elf}
        COMMAND ${CMAKE_C_COMPILER} -fno-pie -no-pie
                -Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1
                -Wl,--hash-style=gnu
                -Wl,--no-as-needed
                -Wl,--allow-shlib-undefined
                -nostdlib -nodefaultlibs
                -o ${_elf}
                ${SBASE_MUSL_CRT}
                -Wl,--start-group ${_obj} ${SBASE_LIBSBASE} -Wl,--end-group
                ${SBASE_LIBC_SO}
                ${SBASE_MUSL_CRTN}
        DEPENDS ${_obj} ${SBASE_LIBSBASE} ${SBASE_LIBC_SO} ${SBASE_MUSL_CRT} ${SBASE_MUSL_CRTN}
        COMMENT "Linking sbase/${_tool}.elf (dynamic libc.so)")
    add_custom_target(${_tgt} ALL DEPENDS ${_elf})
    # musl_libc produces libc.so + Scrt1.o/crti.o/crtn.o; sbase_lib is the
    # compile-time helper archive; musl_headers for the generated bits/*.h.
    add_dependencies(${_tgt} sbase_lib musl_libc musl_headers)

    # Register the ELF in the disk-image manifest → /usr/bin/<tool>.
    os_image_path(${_tgt} sbase_${_tool}.elf usr/bin/${_tool} PARTITION 2)
endforeach()

list(LENGTH SBASE_TOOLS _n)
message(STATUS "sbase: building ${_n} tools → /usr/bin/ (libutf+libutil static, dynamic libc.so, -w)")
