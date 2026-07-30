# libffi build rules — extracted from user/CMakeLists.txt (was lines 1374-1467).
# Shared `ffi_so` (add_third_party_lib) from the third_party/libffi submodule.
# Consumed by wayland (SO_LINK_LIBS ffi); include this before wayland.cmake.

# ===== libffi (third_party/libffi, full-feature incl. closures) — ffi_worklist =====
# fficonfig.h: hand-written fixed template (COPYONLY copy into build tree).
# Pre-defines FFI_MMAP_EXEC_SELINUX/PAX=0 to skip /proc/mounts/statfs probing,
# HAVE_MEMFD_CREATE=1 so closures.c open_temp_exec_file picks memfd first, and
# HAVE_MREMAP=0 (kernel has no mremap). See libffi.md §3.2 / ffi_worklist §1.
configure_file(${CMAKE_SOURCE_DIR}/build_script/libffi/fficonfig.h
               ${CMAKE_BINARY_DIR}/fficonfig.h COPYONLY)

# ffitarget.h: upstream public header (include/Makefile.am:9 nodist_include_HEADERS
# = ffi.h ffitarget.h). ffi.h:86 does `#include <ffitarget.h>`, so every consumer
# of <ffi.h> transitively needs it. The per-arch copies live under src/<arch>/
# (noinst_HEADERS, Makefile.am:53); configure picks the current arch's copy into
# the public include dir. Mirror that: COPYONLY the x86-64 ffitarget.h into the
# build tree next to ffi.h/fficonfig.h, so consumers (test_ffi) and ffi_so itself
# both resolve <ffitarget.h> from the single public dir ${CMAKE_BINARY_DIR} —
# nobody reaches into src/x86/.
configure_file(${CMAKE_SOURCE_DIR}/third_party/libffi/src/x86/ffitarget.h
               ${CMAKE_BINARY_DIR}/ffitarget.h COPYONLY)

# ffi.h: generated from ffi.h.in by @-substitution. The 6 placeholders in
# ffi.h.in are @TARGET@/@HAVE_LONG_DOUBLE@/@FFI_EXEC_TRAMPOLINE_TABLE@/
# @VERSION@/@FFI_VERSION_STRING@/@FFI_VERSION_NUMBER@ (NO LIBFFI_ prefix —
# verified). configure_file substitutes @VAR@ by exact CMake variable name, so
# the variables MUST share the placeholder names. Wrap in a function() so the
# short names TARGET/VERSION do not leak into the parent scope.
function(_libffi_generate_ffi_header)
    set(TARGET X86_64)                       # ffitarget.h:57
    set(VERSION 3.7.1)                       # configure.ac:5 AC_INIT
    set(HAVE_LONG_DOUBLE 1)                  # x86-64 long double = 80-bit x87
    set(FFI_VERSION_STRING "3.7.1")
    set(FFI_VERSION_NUMBER 30701)            # 3*10000 + 7*100 + 1
    set(FFI_EXEC_TRAMPOLINE_TABLE 0)         # no tramp.c → dynamic trampoline
    configure_file(${CMAKE_SOURCE_DIR}/third_party/libffi/include/ffi.h.in
                   ${CMAKE_BINARY_DIR}/ffi.h)
endfunction()
_libffi_generate_ffi_header()

set(LIBFFI_DIR ${CMAKE_SOURCE_DIR}/third_party/libffi)
set(LIBFFI_SOURCES
    ${LIBFFI_DIR}/src/prep_cif.c
    ${LIBFFI_DIR}/src/types.c
    ${LIBFFI_DIR}/src/raw_api.c
    ${LIBFFI_DIR}/src/java_raw_api.c
    ${LIBFFI_DIR}/src/debug.c
    ${LIBFFI_DIR}/src/x86/ffi64.c
    ${LIBFFI_DIR}/src/x86/unix64.S
    ${LIBFFI_DIR}/src/x86/ffiw64.c
    ${LIBFFI_DIR}/src/x86/win64.S
    ${LIBFFI_DIR}/src/closures.c
    ${LIBFFI_DIR}/src/tramp.c
)
set(LIBFFI_INCLUDE_DIRS
    ${CMAKE_BINARY_DIR}              # <ffi.h> <fficonfig.h> <ffitarget.h>(生成/拷贝)
    ${LIBFFI_DIR}/include            # <tramp.h> <ffi_common.h>
    ${LIBFFI_DIR}/src                # dlmalloc.c(closures.c #include)
    ${LIBFFI_DIR}                    # 兜底
)

# libffi.so (shared, full-feature incl. ffi_closure). ffi.h.in's FFI_API is
# an empty define on non-Windows (ffi.h.in:152), so libffi does NOT mark
# exports with visibility("default") — mirror libudev.so: FLAGS adds
# -fvisibility=default to override add_user_lib(SHARED)'s default hidden
# (user_rules.cmake:109), exporting all non-static symbols. -Wno-error
# suppresses WARN_FLAGS' -Werror (dlmalloc is 6443 lines; per-warning
# -Wno-* is impractical) while keeping -Wall -Wextra visible.
# dlmalloc.c is NOT in SOURCES — closures.c:570 #includes it textually.
# tramp.c IS compiled but FFI_EXEC_STATIC_TRAMP is undefined, so only its
# #else stub branch builds (ffi_tramp_is_supported()→0, ffi_tramp_alloc/free
# no-ops). closures.c:901/1003/1029/1050 gate calls on ffi_tramp_is_supported(),
# so the stubs are never hit at runtime — but the link-time symbol refs must
# resolve, else libffi.so's PLT leaves ffi_tramp_free unresolved and the
# dynamic loader FATALs on reloc (was the test_ffi failure).
# ffiw64.c + win64.S provide the FFI_EFI64/FFI_GNUW64 (Windows x64) ABI path.
# ffitarget.h:134 unconditionally #defines FFI_GO_CLOSURES, so ffi64.c's
# ffi_call()/ffi_call_go() emit extern refs to ffi_call_efi64 /
# ffi_call_go_efi64 (plus ffi_prep_cif_machdep_efi64, _closure_loc_efi64,
# _prep_go_closure_efi64) — all defined in ffiw64.c via the EFI64() macro,
# which on non-X86_WIN64 expands to FFI_HIDDEN name##_efi64 (win64.S gives
# the asm backend ffi_call_win64/ffi_closure_win64/ffi_go_closure_win64).
# FFI_DEFAULT_ABI is FFI_UNIX64 here (ffitarget.h:103), so the runtime
# `if (abi == FFI_EFI64 || FFI_GNUW64)` branches are dead — but the link-time
# symbol refs must still resolve, same story as tramp.c. Symbols are
# FFI_HIDDEN so they don't pollute the exported ABI.
add_third_party_lib(ffi_so
    SOURCES ${LIBFFI_SOURCES}
    C
    OUTPUT_NAME ffi
    SO_LINK_LIBS c
    FLAGS "-fvisibility=default -O2"
    INCLUDE_DIRS ${LIBFFI_INCLUDE_DIRS}
    GEN_HEADERS ${CMAKE_BINARY_DIR}/fficonfig.h
                ${CMAKE_BINARY_DIR}/ffi.h
                ${CMAKE_BINARY_DIR}/ffitarget.h
)
