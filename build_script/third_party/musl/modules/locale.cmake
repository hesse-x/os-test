# modules/locale.cmake — musl locale integration (musl_worklist).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl locale integration =====================
# Build the upstream musl locale-management + collation sources into libc, and
# publish musl's <locale.h> (repo had no user/include/locale.h). Scope (decided
# via AskUserQuestion — full POSIX locale management + collate): the 6 locale
# managers (setlocale/localeconv/newlocale/duplocale/freelocale/uselocale) plus
# the 4 collate functions (strcoll/strxfrm/wcscoll/wcsxfrm, each + its _l
# weak_alias). All are pure userspace — no kernel syscall — and all collapse to
# the C locale at runtime (no .mo catalog files / no iconv in this OS), exactly
# matching the existing strerror/langinfo C-locale policy. 0 in-tree callers;
# exported per the ctype/string/wchar "export caller-less POSIX symbols"
# precedent so Mesa/libinput/Proton-style consumers link cleanly.
#
# Sources compiled here (NONE already built elsewhere — verified to avoid
# multi-define):
#   setlocale.c    — setlocale(cat,name): __get_locale + libc.global_locale.
#   localeconv.c   — localeconv(): returns a static POSIX C lconv (no alloc).
#   newlocale.c    — newlocale/__newlocale + DEFINES __loc_is_allocated (used
#                    by freelocale). __get_locale + __libc_malloc.
#   duplocale.c    — duplocale/__duplocale: __libc_malloc.
#   freelocale.c   — freelocale/__freelocale: calls __loc_is_allocated (from
#                    newlocale.c, same module) + __libc_free.
#   uselocale.c    — uselocale/__uselocale: __pthread_self()->locale (the
#                    thread-local locale set by __init_tp in musl_pthread).
#   locale_map.c   — __get_locale (setlocale/newlocale call it): env probe
#                    (getenv LC_ALL/LC_*/LANG) → builtin C/C.UTF-8 fast path;
#                    never finds a catalog (no locale path, MUSL_LOCPATH unset)
#                    → falls back to __c_dot_utf8. DEFINES __locale_lock +
#                    __locale_lockptr (fork_impl.h declares them extern).
#                    Calls __map_file (src/time/__map_file.c, already in
#                    musl_time_objs) + __mo_lookup (built here, next line) +
#                    __libc_malloc + __munmap (musl_glue).
#   __mo_lookup.c  — __mo_lookup (locale_map.c's only catalog dep); pure
#                    compute over a mapped .mo image, never reached in C-locale
#                    mode but linked so locale_map.c resolves.
#   strcoll.c      — strcoll/strcoll_l/__strcoll_l: strcmp(l,r) (code-point).
#   strxfrm.c      — strxfrm/strxfrm_l/__strxfrm_l: strlen+strcpy (code-point).
#   wcscoll.c      — wcscoll/wcscoll_l/__wcscoll_l: wcscmp (musl_wchar_objs).
#   wcsxfrm.c      — wcsxfrm/wcsxfrm_l/__wcsxfrm_l: wcslen+wmemcpy
#                    (musl_wchar_objs).
#
# EXCLUDED (deliberately — i18n-heavy, 0 in-tree callers, out of scope):
#   __lctrans.c / c_locale.c / langinfo.c — ALREADY compiled (musl_pthread
#     __lctrans.c; musl_time_objs c_locale.c + langinfo.c). Re-compiling here
#     multi-defines __lctrans/__lctrans_cur/__lctrans_impl/__c_locale/
#     __c_dot_utf8/__c_dot_utf8_locale/__nl_langinfo_l/nl_langinfo. NOT listed.
#   iconv.c / iconv_close.c — full iconv charset converter tables (codepages/
#     big5/gb18030/jis0208/ksc/hkscs headers); no in-tree consumer, heavy.
#   catopen.c / catgets.c / catclose.c — POSIX catgets message catalog. NOW
#     COMPILED (see below): libc++'s std::messages facet (locale.cpp do_open/
#     do_get/do_close) calls catopen/catgets/catclose. musl's catopen maps the
#     catalog via __map_file + __mo_lookup (both already in libc) and falls back
#     to (nl_catd)-1 when no catalog is found — so the C-locale no-catalog policy
#     holds at runtime, zero new i18n surface beyond the three POSIX entry points.
#   textdomain.c / bind_textdomain_codeset.c / dcngettext.c — gettext/
#     dcgettext/dcngettext/bind_textdomain_codeset (the full GNU gettext API);
#     0 callers, would pull __mo_lookup + catalog mapping chain redundantly.
#   strfmon.c — strfmon/strfmon_l (monetary formatting); needs struct lconv
#     (localeconv provides) but is a standalone heavy formatter, 0 callers.
#   strtod_l.c — strtod_l/strtod (the _l strtod); musl_stdlib_objs already
#     builds strtod.c (the non-_l), and strtod_l would multi-define __strtod_l
#     internals. 0 callers.
#   pleval.c — plural-expression evaluator (gettext `Plural-Forms:` parser);
#     only dcngettext uses it, both excluded.
#
# No UAPI sync TU: <locale.h> carries no OS-specific constants/structs (LC_*
# are small ints, struct lconv is POSIX-standard, locale_t is an opaque
# pointer typedef from <bits/alltypes.h>). install-headers §3p publishes
# musl's <locale.h> verbatim; closure self-check covers it. libc.map exports
# the 6 managers + 4 collate (+_l) under a new <locale.h> block.
add_musl_lib(musl_locale_objs SOURCES
    ${MUSL_DIR}/src/locale/setlocale.c
    ${MUSL_DIR}/src/locale/localeconv.c
    ${MUSL_DIR}/src/locale/newlocale.c
    ${MUSL_DIR}/src/locale/duplocale.c
    ${MUSL_DIR}/src/locale/freelocale.c
    ${MUSL_DIR}/src/locale/uselocale.c
    # __get_locale (called by setlocale/newlocale) + its catalog deps.
    ${MUSL_DIR}/src/locale/locale_map.c
    ${MUSL_DIR}/src/locale/__mo_lookup.c
    # collate (code-point stubs over strcmp/strlen/wcscmp/wcslen/wmemcpy).
    ${MUSL_DIR}/src/locale/strcoll.c
    ${MUSL_DIR}/src/locale/strxfrm.c
    ${MUSL_DIR}/src/locale/wcscoll.c
    ${MUSL_DIR}/src/locale/wcsxfrm.c
    # _l-suffixed numeric conversion (strtof_l/strtod_l/strtold_l) — thin
    # wrappers that ignore the locale arg and call the plain strtof/strtod/
    # strtold (already in musl_stdlib_objs). libc++ locale's __num_get_float
    # calls strtod_l/strtof_l/strtold_l. One file provides all three (weak_alias).
    ${MUSL_DIR}/src/locale/strtod_l.c
    # catgets family — libc++ std::messages facet (locale.cpp do_open/do_get/
    # do_close) calls catopen/catgets/catclose. catopen maps the catalog via
    # __map_file (musl_time_objs) + __mo_lookup (above) + __munmap (musl_glue);
    # returns (nl_catd)-1 when no catalog is found, so C-locale no-catalog policy
    # holds at runtime. catgets/catclose are pure compute over the mapped image.
    ${MUSL_DIR}/src/locale/catopen.c
    ${MUSL_DIR}/src/locale/catgets.c
    ${MUSL_DIR}/src/locale/catclose.c)
