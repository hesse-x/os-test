# modules/compiler_rt.cmake — standalone compiler-rt int128 runtime.
# Included once at the bottom of musl_rules.cmake (NOT in _musl_modules: this
# is compiler-rt, not musl). Provides the 128-bit integer div/mul runtime
# helpers the C/C++ compiler emits implicitly for __int128 arithmetic but
# which neither musl nor our hand-written libc defines.
#
# Why here: libc++'s <filesystem> (operations.cpp file timestamps, int128_builtins.cpp
# __muloti4) performs __int128 division/multiplication, so clang lowers those to
# calls to __divti3 / __udivti3 / __umodti3 / __muloti4. On x86-64 these are NOT
# inline CPU instructions — they are compiler-rt library entry points. musl does
# not ship them. They are supplied by libclang_rt.so, which libc++ records as
# a normal dynamic dependency instead of extending the libc ABI.
#
# Closure (vendored third_party/llvm-project/compiler-rt/lib/builtins/):
#   divti3.c      __divti3  -> __udivmodti4 (via int_div_impl.inc)
#   udivmodti4.c  __udivmodti4 (self-contained Knuth long division)
#   udivti3.c     __udivti3 -> __udivmodti4
#   umodti3.c     __umodti3 -> __udivmodti4
#   muloti4.c     __muloti4 (via int_mulo_impl.inc, self-contained)
# Headers: int_lib.h -> int_types.h + int_endianness.h + int_util.h (macros only).
# All five TUs are pure integer arithmetic — no libc, no syscall, no headers
# beyond the builtins/ dir — so they compile cleanly with just -DCRT_HAS_128BIT
# (x86-64 always has __int128; the macro gates the #ifdef CRT_HAS_128BIT bodies).
# COMPILER_RT_ABI expands to empty on x86-64 (only ARM uses __pcs__("aapcs")).
#
# Built -fPIC as its own DSO. Keeping compiler runtime entry points out of
# libc prevents implementation-specific symbols from becoming libc ABI.
#
# NOTE on flags: uses KERNEL_FREESTANDING_FLAGS (with -isystem ${GCC_FREESTANDING_INC}),
# NOT USER_FREESTANDING_FLAGS — int_lib.h includes <float.h>/<stdint.h>/<limits.h>
# (the standard compiler-bundled freestanding headers), which live in clang's
# resource include dir; USER_FREESTANDING_FLAGS drops -isystem (musl supplies its
# own std*.h), so it would break compiler-rt. The builtins/ dir is prepended via
# target_include_directories so "int_lib.h"/"int_types.h" resolve.
set(COMPILER_RT_DIR ${CMAKE_SOURCE_DIR}/third_party/llvm-project/compiler-rt/lib/builtins)
add_library(compiler_rt_int128 OBJECT
    ${COMPILER_RT_DIR}/divti3.c
    ${COMPILER_RT_DIR}/udivmodti4.c
    ${COMPILER_RT_DIR}/udivti3.c
    ${COMPILER_RT_DIR}/umodti3.c
    ${COMPILER_RT_DIR}/muloti4.c)
target_include_directories(compiler_rt_int128 PRIVATE ${COMPILER_RT_DIR})
target_compile_options(compiler_rt_int128 PRIVATE
    -m64 ${KERNEL_FREESTANDING_FLAGS} -fPIC -DCRT_HAS_128BIT -Wno-everything
    ${THIRD_PARTY_OPT_FLAGS})
