# modules/math.cmake — musl math integration (libm).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl math integration (libm) =====================
# Build the upstream musl src/math/*.c + x86_64/*.s + fenv/x86_64/fenv.s into
# libm, replacing the hand-written __builtin_* wrappers in the deleted
# user/lib/math/*.c. Mirrors musl_string_objs: one -fPIC OBJECT sub-library
# (add_musl_lib) wired into BOTH the static libm.a (m) and shared libm.so
# (m_so) via EXTRA_OBJS — -fPIC objects link fine into a -no-pie static archive
# too, so a single compile serves both (same as musl_string_objs → c/c_so).
#
# src/math/*.c is one-function-per-file. The x86_64/*.s asm versions REPLACE the
# C ones for 29 symbols (acosl asinl atan2l atanl ceill exp2l expl expm1l fabs
# fabsf fabsl floorl fmodl llrint llrintf llrintl log10l log1pl log2l logl lrint
# lrintf lrintl remainderl rintl sqrt sqrtf sqrtl truncl, plus __invtrigl) —
# compiling both the .c and .s would multi-define those symbols, so the .c
# twins are dropped from the glob (same pattern as MUSL_STRING_SOURCES drops the
# .c that have a .s twin). The asm .s set is added explicitly via GLOB below.
#
# fenv: src/math/{nearbyint,fma,fmaf,fmal,llrint,...}.c #include <fenv.h> and
# call feclearexcept/feraiseexcept/fetestexcept. On x86_64 musl implements the
# entire fenv ABI in one assembly file — src/fenv/x86_64/fenv.s — so pull it in
# to satisfy those references. musl <fenv.h> needs <bits/fenv.h> (published via
# install-headers.sh), already on this target's musl arch include path.
#
# Internal helpers (__fpclassify* __signbit* __math_* __polevll __sinl __cosl
# __tanl __rem_pio2* __expo2* __signgam __lgamma_r ...) are all caught by the
# src/math/__*.c glob and stay hidden (not in libm.map). The only external
# reference math sources make is errno via __errno_location (provided by
# musl_pthread in libc), resolved at program link time.
file(GLOB MUSL_MATH_SOURCES ${MUSL_DIR}/src/math/*.c)
list(REMOVE_ITEM MUSL_MATH_SOURCES
    # C versions superseded by the x86_64 .s asm below — keep them out or the
    # link multi-defines these symbols.
    ${MUSL_DIR}/src/math/acosl.c
    ${MUSL_DIR}/src/math/asinl.c
    ${MUSL_DIR}/src/math/atan2l.c
    ${MUSL_DIR}/src/math/atanl.c
    ${MUSL_DIR}/src/math/ceill.c
    ${MUSL_DIR}/src/math/exp2l.c
    ${MUSL_DIR}/src/math/expl.c
    ${MUSL_DIR}/src/math/expm1l.c
    ${MUSL_DIR}/src/math/fabs.c
    ${MUSL_DIR}/src/math/fabsf.c
    ${MUSL_DIR}/src/math/fabsl.c
    ${MUSL_DIR}/src/math/floorl.c
    ${MUSL_DIR}/src/math/fmodl.c
    ${MUSL_DIR}/src/math/__invtrigl.c
    ${MUSL_DIR}/src/math/llrint.c
    ${MUSL_DIR}/src/math/llrintf.c
    ${MUSL_DIR}/src/math/llrintl.c
    ${MUSL_DIR}/src/math/log10l.c
    ${MUSL_DIR}/src/math/log1pl.c
    ${MUSL_DIR}/src/math/log2l.c
    ${MUSL_DIR}/src/math/logl.c
    ${MUSL_DIR}/src/math/lrint.c
    ${MUSL_DIR}/src/math/lrintf.c
    ${MUSL_DIR}/src/math/lrintl.c
    ${MUSL_DIR}/src/math/remainderl.c
    ${MUSL_DIR}/src/math/rintl.c
    ${MUSL_DIR}/src/math/sqrt.c
    ${MUSL_DIR}/src/math/sqrtf.c
    ${MUSL_DIR}/src/math/sqrtl.c
    ${MUSL_DIR}/src/math/truncl.c)
file(GLOB MUSL_MATH_ASM_SOURCES ${MUSL_DIR}/src/math/x86_64/*.s)
list(APPEND MUSL_MATH_SOURCES
    ${MUSL_MATH_ASM_SOURCES}
    ${MUSL_DIR}/src/fenv/x86_64/fenv.s
    # Generic fenv wrappers NOT provided by x86_64/fenv.s. The .s defines the
    # low-level feclearexcept/feraiseexcept/fetestexcept/fegetenv/fesetenv/
    # fegetround/__fesetround; these generic .c wrap __fesetround/fegetenv/etc.
    # to provide the remaining public fenv API. fenv.c is EXCLUDED — it defines
    # feclearexcept/etc. that the .s already provides (multi-def). __flt_rounds
    # backs the FLT_ROUNDS macro in <float.h>.
    ${MUSL_DIR}/src/fenv/fesetround.c
    ${MUSL_DIR}/src/fenv/fegetexceptflag.c
    ${MUSL_DIR}/src/fenv/fesetexceptflag.c
    ${MUSL_DIR}/src/fenv/feholdexcept.c
    ${MUSL_DIR}/src/fenv/feupdateenv.c
    ${MUSL_DIR}/src/fenv/__flt_rounds.c)
add_musl_lib(musl_math_objs SOURCES ${MUSL_MATH_SOURCES})
