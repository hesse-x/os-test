# modules/string.cmake — musl string integration (string.md).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl string integration (string.md) =====================
# Build the upstream musl src/string/*.c into libc, replacing hand-written
# user/lib/string.cc. Pure-compute module (zero syscall dependency) — the
# "compile-through" tier in libc_worklist §A and a prerequisite for the wchar/
# locale tiers. Benefits: x86_64 rep movsq/stosq asm memcpy/memmove/memset
# (vs hand-written byte loops), canonical strerror/strerror_r, and the full
# POSIX/GNU extension set (stpcpy/stpncpy/strlcpy/strlcat/strsep/memccpy/
# mempcpy/memrchr/strchrnul/strcasestr/strnlen/strverscmp/swab/ffsl/ffsll/...).
#
# src/string/*.c is one-function-per-file. The x86_64/{memcpy,memmove,memset}.s
# asm versions REPLACE the C ones — compiling both would multi-define those
# three symbols (the .c and .s each define memcpy/memmove/memset), so the .c
# trio is dropped from the glob (same pattern as MUSL_THREAD_SOURCES drops the
# .c that have a .s twin). strerror/strsignal route messages through LCTRANS/
# CURRENT_LOCALE, but strerror.c + __lctrans.c are ALREADY compiled into
# musl_pthread (commit 3902dfc moved strerror there) — they are NOT re-added
# here to avoid a duplicate-definition clash. strsignal.c is excluded too:
# 0 callers, and its locale mechanism (__lctrans/__lctrans_cur) is already
# pulled in by strerror in musl_pthread. strcoll/strxfrm/strerror_l/strcoll_l/
# strxfrm_l/strcasecmp_l/strncasecmp_l live in src/locale/ and need the full
# locale subsystem — not compiled, so musl <string.h> declares them with no
# implementation (0 callers; deferred to the locale tier). Wide-character
# wcs*/wmem* (and wcpcpy/wcpncpy) are excluded here — built in musl_wchar_objs
# (wchar tier). See the musl_wchar_objs block above for the full set.
file(GLOB MUSL_STRING_SOURCES ${MUSL_DIR}/src/string/*.c)
list(REMOVE_ITEM MUSL_STRING_SOURCES
    # C versions superseded by the x86_64 .s asm below — keep them out or the
    # link multi-defines memcpy/memmove/memset.
    ${MUSL_DIR}/src/string/memcpy.c
    ${MUSL_DIR}/src/string/memmove.c
    ${MUSL_DIR}/src/string/memset.c
    # 0 callers; locale machinery already introduced by strerror in musl_pthread.
    ${MUSL_DIR}/src/string/strsignal.c
    # Wide-character set — built in musl_wchar_objs (wchar tier), not here.
    ${MUSL_DIR}/src/string/wcpcpy.c
    ${MUSL_DIR}/src/string/wcpncpy.c)
# Drop the remaining wcs*/wmem* files caught by the glob (GLOB can't express
# the prefix wildcard inside REMOVE_ITEM reliably, so filter by filename).
foreach(_src ${MUSL_STRING_SOURCES})
    get_filename_component(_name ${_src} NAME)
    if(_src MATCHES "/(wcs|wmem)" OR _name MATCHES "^(wcs|wmem)")
        list(REMOVE_ITEM MUSL_STRING_SOURCES ${_src})
    endif()
endforeach()
list(APPEND MUSL_STRING_SOURCES
    ${MUSL_DIR}/src/string/x86_64/memcpy.s
    ${MUSL_DIR}/src/string/x86_64/memmove.s
    ${MUSL_DIR}/src/string/x86_64/memset.s
    # ffs/ffsl/ffsll are in src/misc/ (declared in <strings.h>); basename +
    # dirname are in src/misc/ (POSIX libgen.h, replacing the hand-written
    # strrchr+1 basename in the deleted user/lib/string.cc). dirname is pulled
    # in alongside basename so musl's libgen.h (which declares both) links
    # cleanly — libinput util-files.h calls dirname().
    ${MUSL_DIR}/src/misc/ffs.c
    ${MUSL_DIR}/src/misc/ffsl.c
    ${MUSL_DIR}/src/misc/ffsll.c
    ${MUSL_DIR}/src/misc/basename.c
    ${MUSL_DIR}/src/misc/dirname.c)
add_musl_lib(musl_string_objs SOURCES ${MUSL_STRING_SOURCES})
