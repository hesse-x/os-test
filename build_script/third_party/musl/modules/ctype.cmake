# modules/ctype.cmake — musl narrow-ctype integration (musl_worklist).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl narrow-ctype integration =====================
# Build the upstream musl narrow-ctype sources into libc, replacing the
# hand-written user/lib/ctype.c (deleted this batch). The wide-character
# isw*/wctype/towctrans/wcwidth set is already built in musl_wchar_objs
# (wchar.cmake); this module covers ONLY the narrow <ctype.h> surface.
#
# Each narrow per-function file (isalnum.c/isalpha.c/.../tolower.c/toupper.c
# plus isascii.c/toascii.c) is a one-expression inline compute that only
# #includes <ctype.h> and defines both the plain symbol and a __<fn>_l /
# <fn>_l weak_alias pair. They do NOT call the __ctype_b_loc /
# __ctype_tolower_loc / __ctype_toupper_loc table providers — every narrow
# function computes its result directly — so those four provider files are
# excluded (they would only add unreferenced symbols, and
# __ctype_get_mb_cur_max.c would pull locale_impl.h/CURRENT_UTF8). locale_t
# appears only as an _l parameter type, resolved via <ctype.h> ->
# __NEED_locale_t -> bits/alltypes.h (already generated), so no src/locale
# file is pulled in. No multi-define clash: nothing else compiles these
# narrow files (musl_wchar_objs builds only the wide set).
#
# musl <ctype.h> (now the published header, verbatim from musl/include) is a
# strict superset of the deleted repo shim: it adds the inline-macro
# optimizations for isalpha/isdigit/islower/isupper/isprint/isgraph/isspace
# (the .c files #undef the matching macro before defining the function) plus
# the 14 _l locale_t variants and isascii/toascii/_tolower/_toupper under the
# _POSIX/_XOPEN gate. add_musl_lib bakes in -D_XOPEN_SOURCE=700, which alone
# satisfies ctype.h's POSIX gate, making all of those visible. The 15 plain
# symbols are already in libc.map; this batch adds the 14 _l exports.
add_musl_lib(musl_ctype_objs SOURCES
    ${MUSL_DIR}/src/ctype/isalnum.c
    ${MUSL_DIR}/src/ctype/isalpha.c
    ${MUSL_DIR}/src/ctype/isblank.c
    ${MUSL_DIR}/src/ctype/iscntrl.c
    ${MUSL_DIR}/src/ctype/isdigit.c
    ${MUSL_DIR}/src/ctype/isgraph.c
    ${MUSL_DIR}/src/ctype/islower.c
    ${MUSL_DIR}/src/ctype/isprint.c
    ${MUSL_DIR}/src/ctype/ispunct.c
    ${MUSL_DIR}/src/ctype/isspace.c
    ${MUSL_DIR}/src/ctype/isupper.c
    ${MUSL_DIR}/src/ctype/isxdigit.c
    ${MUSL_DIR}/src/ctype/tolower.c
    ${MUSL_DIR}/src/ctype/toupper.c
    ${MUSL_DIR}/src/ctype/isascii.c
    ${MUSL_DIR}/src/ctype/toascii.c)
