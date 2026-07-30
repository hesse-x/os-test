# modules/multibyte.cmake — musl multibyte subset (narrow stdio %ls/%lc only).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl multibyte subset (narrow stdio %ls/%lc only) ====================
# vfprintf.c references wctomb (%ls/%lc wide-string formatting) and vfscanf.c
# references mbrtowc + mbsinit (%ls scanning) — these are link-time refs in the
# narrow-char printf/scanf path, so they must be satisfied. They live in musl
# src/multibyte, which no other musl_*_objs compiles. We compile only the minimal
# narrow-path closure, NOT the whole src/multibyte glob:
#   wctomb.c  → calls wcrtomb
#   wcrtomb.c → UTF-8 encoder (MB_CUR_MAX resolves inline; no link ref to bittab)
#   mbrtowc.c → UTF-8 decoder, references bittab (the __fsmu8 table)
#   mbsinit.c → pure (no deps)
#   internal.c → provides bittab (__fsmu8)
# The OTHER multibyte files (btowc/wctob/mbtowc/mblen/mbsrtowcs/wcsrtombs/...) are
# NOT compiled: they are only referenced by the wide-char stdio files (vfwprintf/
# fgetwc/...), which are excluded from musl_stdio_objs above. Including them would
# pull the C-locale objects __c_locale/__c_dot_utf8_locale (src/locale/c_locale.c)
# and recreate the runtime reloc failure. MB_CUR_MAX → (CURRENT_UTF8 ? 4 : 1) reads
# __pthread_self()->locale at runtime (set by __init_tp in musl_pthread) — no link
# dep on __c_locale. No public API here is exported (backs stdio internals).
add_musl_lib(musl_multibyte_objs SOURCES
    ${MUSL_DIR}/src/multibyte/wctomb.c
    ${MUSL_DIR}/src/multibyte/wcrtomb.c
    ${MUSL_DIR}/src/multibyte/mbrtowc.c
    ${MUSL_DIR}/src/multibyte/mbsinit.c
    ${MUSL_DIR}/src/multibyte/internal.c)
