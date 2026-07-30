# musl subproject build — ldso.md Phase 0.
#
# Builds ONLY the musl objects the fused libc.so / ELF links consume, via CMake
# (NOT musl's own ./configure && make — that compiled all of musl as unused
# waste; musl's generated bits/alltypes.h + bits/syscall.h are checked into
# user/include/bits/, so no configure step is needed at all).
#
# Produces:
#   musl_loader_objs  — OBJECT lib: ldso/dynlink.c + ldso/dlstart.c, -fPIC.
#                        Spliced into the fused libc.so via $<TARGET_OBJECTS:...>
#                        (ldso.md §0/§2). _dlstart is the .so entry (-Wl,-e).
#   ${MUSL_LIB_DIR}/crt1.o  — static _start (crt/crt1.c, -DCRT, non-PIC)
#   ${MUSL_LIB_DIR}/Scrt1.o — dynamic _start (crt/Scrt1.c, -DCRT, -fPIC)
#   ${MUSL_LIB_DIR}/crti.o  — .init bracket (crt/x86_64/crti.s)
#   ${MUSL_LIB_DIR}/crtn.o  — .fini bracket (crt/x86_64/crtn.s)
#
# The hand-written libc (user/lib) is fused with the loader objects into a single
# libc.so (ldso.md §0/§2). Downstream targets depend on the `musl_libc` target and
# reference ${MUSL_LIB_DIR} for the crt .o. musl's own libc.a/libc.so are NOT
# built — we have our own hand-written libc.a and the fused libc.so.

set(MUSL_SRC ${CMAKE_SOURCE_DIR}/third_party/musl)
set(MUSL_BUILD ${CMAKE_BINARY_DIR}/musl)
set(MUSL_LIB_DIR ${MUSL_BUILD}/lib)

# Generate musl's build-time headers (bits/alltypes.h + bits/syscall.h) and
# export MUSL_GEN_INCLUDE_DIR to this (top-level) directory scope BEFORE any
# add_musl_lib / raw musl target is defined. Previously called from
# user/CMakeLists.txt (pthread block); moved up so the 14 libc OBJECT libs
# (modules/*.cmake, included at the bottom of this file) see it.
musl_generate_headers()
# Alias used by modules/*.cmake source lists (mirrors the MUSL_DIR that used to
# be set in user/CMakeLists.txt). Same value as MUSL_SRC.
set(MUSL_DIR ${MUSL_SRC})

# musl-internal include order: musl src/internal BEFORE user/include so musl
# sources' quoted #include "syscall.h"/"libc.h" resolve to musl's own headers.
# arch/x86_64 provides syscall_arch.h; arch/generic is the bits fallback.
# src/include precedes src/internal per musl's Makefile (-Isrc/include
# -Isrc/internal): since 1.2.x, src/internal/syscall.h #includes <features.h>,
# and the internal macros hidden/weak/weak_alias live ONLY in
# src/include/features.h (v1.1.19 had no hidden keyword; weak_alias was defined
# in libc.h). Without src/include, <features.h> resolves to user/include and
# every musl source errors with "unknown type name 'hidden'".
# user/include carries the pre-generated bits/syscall.h + bits/alltypes.h +
# xos headers. Relaxed warnings (-Wno-all): upstream musl is third-party code,
# not under our -Werror gate (same rationale as musl_unistd_objs).
# MUSL_GEN_INCLUDE_DIR (build/musl_gen, from musl_generate_headers above) is
# FIRST so <bits/alltypes.h> resolves to the v1.2.6-generated copy — it defines
# __LONG_MAX, which musl 1.2.x <limits.h> references (#define LONG_MAX
# __LONG_MAX). Our static user/include/bits/alltypes.h is a v1.1.19-era hand
# write (no __LONG_MAX), so without musl_gen first, ldso/dynlink.c fails with
# "use of undeclared identifier '__LONG_MAX'". Mirrors add_musl_lib's order.
set(MUSL_INCLUDES
    ${MUSL_GEN_INCLUDE_DIR}
    ${MUSL_SRC}/arch/x86_64
    ${MUSL_SRC}/arch/generic
    ${MUSL_SRC}/src/include
    ${MUSL_SRC}/src/internal
    ${MUSL_SRC}/include
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)

# ===================== loader objects (PIC, spliced into libc.so) =====================
# Mirrors musl_unistd_objs_so: same musl-internal include order, same -fPIC for
# the shared link. dynlink.c is the dynamic linker (fused per ldso.md §0);
# dlstart.c is the arch bootstrap that calls __dls2. Their musl-internal
# references (__libc, __hwcap, __init_tp, __copy_tls, __block_all_sigs, ...) are
# satisfied at link time by the musl_pthread objects (spliced into libc.so via
# EXTRA_OBJS); the loader-only, non-pthread symbols (getdelim/
# __tlsdesc_*/__libc_get_version/__dl_vseterr) come from lib/musl_loader_shim.c.
# (dprintf/vdprintf are NOT shim-provided — the loader resolves them to musl's
# native src/stdio/{d,vd}printf.c, which is safe since every loader call site
# runs after reloc_all(&ldso).)
add_library(musl_loader_objs OBJECT
    ${MUSL_SRC}/ldso/dynlink.c
    ${MUSL_SRC}/ldso/dlstart.c)
target_include_directories(musl_loader_objs PRIVATE ${MUSL_INCLUDES})
# Unlike add_musl_lib (which wires musl_headers internally), this is a raw
# add_library, so the generated-header dependency must be added by hand: dynlink.c
# pulls musl <limits.h> → LONG_MAX → __LONG_MAX, defined ONLY in the generated
# bits/alltypes.h (build/musl_gen/bits/, from arch/x86_64/bits/alltypes.h.in).
# The static user/include/bits/alltypes.h fallback has no __LONG_MAX. v1.1.19's
# dynlink.c never referenced __LONG_MAX so the missing dep was latent; v1.2.6's
# does (the SSIZE_MAX/PATH_MAX and n_th overflow checks), and without this dep a
# fast/parallel build can compile the loader before alltypes.h is generated,
# falling back to the static header and failing with "undeclared __LONG_MAX".
add_dependencies(musl_loader_objs musl_headers)
# Force -O2 regardless of CMAKE_BUILD_TYPE. The loader is bootstrap-critical:
# at -O0 clang lowers every aggregate zero-init (e.g. `struct symdef def = {0}`
# in find_sym, dynlink.c:264) to a `call memset@plt`. But reloc_all(&ldso)
# resolves ldso's OWN PLT GOT slots via find_sym — so the memset GOT slot is
# still the un-relocated file value when find_sym runs, and the call jumps to
# an unmapped wild address (0x6476). Upstream musl builds ldso at -Os; -O2
# likewise emits inline stores and avoids the self-referential PLT call.
# -fno-builtin / -fno-tree-loop-distribute-patterns do NOT fix this at -O0.
target_compile_options(musl_loader_objs PRIVATE
    -m64 ${FREESTANDING_FLAGS} -fPIC -Wno-all -O2
    -std=c99 -D_XOPEN_SOURCE=700 -Wno-error
    # -Wno-all ≠ -w: clang does not group -Wempty-body under -Wall, so the
    # `for (...); auxv++;` idiom in dynlink.c:1394 still warns under -Wno-all.
    -Wno-empty-body)
# Scrt1.c includes crt1.c; dynlink.c pulls __libc / struct definitions that
# must see musl's libc.h — no extra defines needed beyond the include order.

# ===================== crt objects (standalone .o, consumed by file path) =====================
# ELF links (add_user_elf / add_user_dyn_elf) bracket the program objects with
# crt1.o/Scrt1.o (entry _start → _start_c → __libc_start_main) and crti.o/crtn.o
# (.init/.fini section brackets). These are bare-gcc custom commands producing
# individual .o files at ${MUSL_LIB_DIR}, referenced by path in user_rules.cmake.
file(MAKE_DIRECTORY ${MUSL_LIB_DIR})

# Shared compile flags for the C crt sources (crt1.c / Scrt1.c). -DCRT selects
# musl's _start/_start_c path (crt1.c is also #included by Scrt1.c with CRT
# defined). crt1.o is non-PIC (-fno-pie) for the static link; Scrt1.o is -fPIC.
set(_crt_c_flags -m64 ${FREESTANDING_FLAGS} -DCRT -Wno-all)
foreach(inc ${MUSL_INCLUDES})
    list(APPEND _crt_c_inc -I${inc})
endforeach()

add_custom_command(
    OUTPUT ${MUSL_LIB_DIR}/crt1.o
    COMMAND ${CMAKE_C_COMPILER} ${_crt_c_flags} ${_crt_c_inc}
                -fno-pie -c ${MUSL_SRC}/crt/crt1.c
                -o ${MUSL_LIB_DIR}/crt1.o
    DEPENDS ${MUSL_SRC}/crt/crt1.c musl_headers
    COMMENT "musl crt1.o (static _start, -DCRT)"
    VERBATIM)

add_custom_command(
    OUTPUT ${MUSL_LIB_DIR}/Scrt1.o
    COMMAND ${CMAKE_C_COMPILER} ${_crt_c_flags} ${_crt_c_inc}
                -fPIC -c ${MUSL_SRC}/crt/Scrt1.c
                -o ${MUSL_LIB_DIR}/Scrt1.o
    DEPENDS ${MUSL_SRC}/crt/Scrt1.c ${MUSL_SRC}/crt/crt1.c musl_headers
    COMMENT "musl Scrt1.o (dynamic _start, -DCRT -fPIC)"
    VERBATIM)

# crti.s / crtn.s are arch asm (.init/.fini brackets); assembled directly.
# -Wa,--noexecstack injects an empty .note.GNU-stack into the object so the
# linker doesn't infer an executable stack (musl's bare .s files carry no such
# note; without this, every ELF link warns about crtn.o/crti.o).
add_custom_command(
    OUTPUT ${MUSL_LIB_DIR}/crti.o
    COMMAND ${CMAKE_C_COMPILER} -m64 -Wa,--noexecstack -c ${MUSL_SRC}/crt/x86_64/crti.s
                -o ${MUSL_LIB_DIR}/crti.o
    DEPENDS ${MUSL_SRC}/crt/x86_64/crti.s
    COMMENT "musl crti.o (.init bracket)"
    VERBATIM)

add_custom_command(
    OUTPUT ${MUSL_LIB_DIR}/crtn.o
    COMMAND ${CMAKE_C_COMPILER} -m64 -Wa,--noexecstack -c ${MUSL_SRC}/crt/x86_64/crtn.s
                -o ${MUSL_LIB_DIR}/crtn.o
    DEPENDS ${MUSL_SRC}/crt/x86_64/crtn.s
    COMMENT "musl crtn.o (.fini bracket)"
    VERBATIM)

# Aggregate target: consumers do `add_dependencies(<tgt> musl_libc)` to ensure
# the crt .o exist before linking. (The loader objects reach libc.so via
# $<TARGET_OBJECTS:musl_loader_objs>, which carries its own dependency.)
add_custom_target(musl_libc ALL
    DEPENDS ${MUSL_LIB_DIR}/crt1.o
            ${MUSL_LIB_DIR}/Scrt1.o
            ${MUSL_LIB_DIR}/crti.o
            ${MUSL_LIB_DIR}/crtn.o)

# ===================== libc OBJECT libs (14 modules, fed into libc.a/libc.so) =====================
# Each module file defines one or two musl OBJECT sub-libraries (source list +
# exclude + add_musl_lib / raw add_library), moved out of user/CMakeLists.txt.
# Order matches the original inline layout (unistd → time); CMake resolves
# target deps at generate time so order is for readability, not correctness —
# but musl_generate_headers() above MUST run before any add_musl_lib here.
set(_musl_modules
    unistd fcntl socket dl dirent mman
    stdio multibyte wchar pthread string math stdlib malloc time)
foreach(_m ${_musl_modules})
    include(${CMAKE_CURRENT_LIST_DIR}/modules/${_m}.cmake)
endforeach()
