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
# src/math/*.c is one-function-per-file. For some symbols musl ships an arch
# override under src/math/$(ARCH)/<name>.[csS] (assembly OR a hand-tuned C
# variant) that REPLACES the generic src/math/<name>.c — compiling both would
# multi-define the symbol. musl's Makefile handles this generically
# (REPLACED_OBJS = $(subst /$(ARCH)/,/,$(ARCH_OBJS)); ALL_OBJS = filter-out
# $(REPLACED_OBJS) $(BASE_OBJS)): every arch file src/math/$(ARCH)/<name>.*
# shadows src/math/<name>.c, whatever its extension. We mirror that exactly
# below: glob the arch dir, derive each replaced base .c, and REMOVE_ITEM it
# from the base glob. This is version-robust — musl 1.2.x converted ~15 of the
# v1.1.19 .s overrides back to .c (fabs/fabsf/fabsl/fmodl/llrint*/lrint*/
# remainderl/rintl/sqrt/sqrtf/sqrtl/fma/fmaf...), and a hand-maintained exclude
# list (the old form here) would silently drop symbols whose .s no longer
# exists, leaving fabs/sqrt/... undefined (caught only by the libc.map export
# check). The auto-derived set always tracks what the arch dir actually ships.
# (ARCH is x86_64 — the only target this OS supports; kept literal to match the
# other modules rather than threaded through a variable.)
file(GLOB MUSL_MATH_SOURCES ${MUSL_DIR}/src/math/*.c)
file(GLOB MUSL_MATH_ARCH_SOURCES ${MUSL_DIR}/src/math/x86_64/*.[csS])
# For each arch override, drop the generic .c twin it replaces. get_filename
# name without extension → src/math/<name>.c (basename match, arch-extension
# agnostic: a .s OR .c override both suppress the base .c).
set(_math_replaced)
foreach(_arch_src ${MUSL_MATH_ARCH_SOURCES})
    get_filename_component(_name ${_arch_src} NAME_WE)
    list(APPEND _math_replaced ${MUSL_DIR}/src/math/${_name}.c)
endforeach()
list(REMOVE_ITEM MUSL_MATH_SOURCES ${_math_replaced})
list(APPEND MUSL_MATH_SOURCES ${MUSL_MATH_ARCH_SOURCES})
list(APPEND MUSL_MATH_SOURCES
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
