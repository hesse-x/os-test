# modules/wchar.cmake — musl wchar/wctype/uchar integration (wchar tier).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ============== musl wchar/wctype/uchar integration (wchar tier) ==============
# Single -fPIC OBJECT lib (add_musl_lib) feeds BOTH libc.a + libc.so (one compile
# serves both, same as musl_string_objs/musl_stdlib_objs). Publishes <wchar.h>/
# <wctype.h>/<uchar.h> with zero in-tree consumers — pure additive ABI, no
# regression surface. DEFERRED:
#   wide-char stdio (23 w* files) — kept excluded in musl_stdio_objs (no consumer);
#   wcscoll/wcsxfrm (+_l) — need src/locale (not built), declare-only (libc.map
#   does not list, not compiled, 0 callers), same treatment as strcoll/strxfrm.
# internal.c/wctomb.c/wcrtomb.c/mbrtowc.c/mbsinit.c are NOT re-listed here (in
# musl_multibyte_objs) — recompiling would multi-define bittab/wcrtomb/mbrtowc/
# mbsinit/wctomb. Narrow ctype (isalnum/tolower/...) stays in user/lib/ctype.c
# (libc.map already exports) — NOT compiled here.
add_musl_lib(musl_wchar_objs SOURCES
    # <uchar.h>: 4 thin wrappers, built on wcrtomb/mbrtowc (in musl_multibyte_objs)
    ${MUSL_DIR}/src/multibyte/c16rtomb.c
    ${MUSL_DIR}/src/multibyte/mbrtoc16.c
    ${MUSL_DIR}/src/multibyte/c32rtomb.c
    ${MUSL_DIR}/src/multibyte/mbrtoc32.c
    # <wctype.h>: isw* classification + iswctype/wctype/towctrans/wctrans/towupper/towlower + _l
    ${MUSL_DIR}/src/ctype/iswalnum.c
    ${MUSL_DIR}/src/ctype/iswalpha.c
    ${MUSL_DIR}/src/ctype/iswblank.c
    ${MUSL_DIR}/src/ctype/iswcntrl.c
    ${MUSL_DIR}/src/ctype/iswdigit.c
    ${MUSL_DIR}/src/ctype/iswgraph.c
    ${MUSL_DIR}/src/ctype/iswlower.c
    ${MUSL_DIR}/src/ctype/iswprint.c
    ${MUSL_DIR}/src/ctype/iswpunct.c
    ${MUSL_DIR}/src/ctype/iswspace.c
    ${MUSL_DIR}/src/ctype/iswupper.c
    ${MUSL_DIR}/src/ctype/iswxdigit.c
    ${MUSL_DIR}/src/ctype/iswctype.c
    ${MUSL_DIR}/src/ctype/wctrans.c
    ${MUSL_DIR}/src/ctype/towctrans.c
    ${MUSL_DIR}/src/ctype/wcwidth.c
    ${MUSL_DIR}/src/ctype/wcswidth.c
    # <wchar.h> wcs*/wmem* string set (musl_string_objs still excludes them; built
    # separately here to avoid disturbing the stable string block)
    ${MUSL_DIR}/src/string/wcscpy.c
    ${MUSL_DIR}/src/string/wcsncpy.c
    ${MUSL_DIR}/src/string/wcscat.c
    ${MUSL_DIR}/src/string/wcsncat.c
    ${MUSL_DIR}/src/string/wcscmp.c
    ${MUSL_DIR}/src/string/wcsncmp.c
    ${MUSL_DIR}/src/string/wcschr.c
    ${MUSL_DIR}/src/string/wcsrchr.c
    ${MUSL_DIR}/src/string/wcscspn.c
    ${MUSL_DIR}/src/string/wcsspn.c
    ${MUSL_DIR}/src/string/wcspbrk.c
    ${MUSL_DIR}/src/string/wcstok.c
    ${MUSL_DIR}/src/string/wcslen.c
    ${MUSL_DIR}/src/string/wcsnlen.c
    ${MUSL_DIR}/src/string/wcsstr.c
    ${MUSL_DIR}/src/string/wcswcs.c
    ${MUSL_DIR}/src/string/wcscasecmp.c
    ${MUSL_DIR}/src/string/wcsncasecmp.c
    ${MUSL_DIR}/src/string/wcscasecmp_l.c
    ${MUSL_DIR}/src/string/wcsncasecmp_l.c
    ${MUSL_DIR}/src/string/wcsdup.c
    ${MUSL_DIR}/src/string/wcpcpy.c
    ${MUSL_DIR}/src/string/wcpncpy.c
    ${MUSL_DIR}/src/string/wmemchr.c
    ${MUSL_DIR}/src/string/wmemcmp.c
    ${MUSL_DIR}/src/string/wmemcpy.c
    ${MUSL_DIR}/src/string/wmemmove.c
    ${MUSL_DIR}/src/string/wmemset.c
    # <wchar.h> remaining multibyte (wctomb/wcrtomb/mbrtowc/mbsinit/internal already
    # in musl_multibyte_objs, not re-listed)
    ${MUSL_DIR}/src/multibyte/btowc.c
    ${MUSL_DIR}/src/multibyte/wctob.c
    ${MUSL_DIR}/src/multibyte/mblen.c
    ${MUSL_DIR}/src/multibyte/mbrlen.c
    ${MUSL_DIR}/src/multibyte/mbstowcs.c
    ${MUSL_DIR}/src/multibyte/wcstombs.c
    ${MUSL_DIR}/src/multibyte/mbsrtowcs.c
    ${MUSL_DIR}/src/multibyte/wcsrtombs.c
    ${MUSL_DIR}/src/multibyte/mbsnrtowcs.c
    ${MUSL_DIR}/src/multibyte/wcsnrtombs.c
    ${MUSL_DIR}/src/multibyte/mbtowc.c
    # <wchar.h> wide numeric conversion (__intscan/__floatscan/shlim/shcnt from
    # musl_stdlib_objs resolve via the merged libc.a/libc.so; wcstol.c derefs
    # struct _IO_FILE fields, resolved by src/internal/stdio_impl.h ahead in path)
    ${MUSL_DIR}/src/stdlib/wcstol.c
    ${MUSL_DIR}/src/stdlib/wcstod.c
    # <wchar.h> wcsftime (time machinery from musl_time_objs; see todo.md for the
    # __strftime_fmt_1 signature-mismatch upstream bug — compiled verbatim)
    ${MUSL_DIR}/src/time/wcsftime.c)
