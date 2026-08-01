# modules/time.cmake — musl time integration (time.md).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl time integration (time.md) =====================
# Build the upstream musl src/time/*.c into libc, mirroring musl_string_objs /
# musl_math_objs / musl_stdlib_objs (one -fPIC OBJECT sub-library via add_musl_lib,
# wired into BOTH libc.a and libc.so via EXTRA_OBJS — -fPIC objects link fine
# into a -no-pie static ELF too, so a single compile serves both).
#
# This batch RETIRES the repo's hand-written user/lib/time.cc (slimmed to ONLY
# sleep/usleep, which are deliberately retained — see below): clock_gettime,
# time, gettimeofday, clock/clock_settime/clock_nanosleep/nanosleep, gmtime/
# localtime/mktime/timegm, asctime/ctime, strftime/strptime, difftime,
# timespec_get, and the timezone stubs all come from musl upstream now.
#
# Kernel syscall coverage (include/uapi/xos/syscall_nums.h + kernel/bsd/
# syscall.c dispatch): SYS_clock_gettime(228)/SYS_clock_settime(227)/
# SYS_clock_nanosleep(230)/SYS_nanosleep(35)/SYS_gettimeofday(96)/
# SYS_utimensat(280) are implemented. SYS_clock_getres/SYS_times have NO
# kernel impl — musl's clock_getres.c/times.c route through __syscall and
# return -ENOSYS (errno) at runtime; 0 callers, acceptable. SYS_timer_* are
# also unimplemented → the timer_create family is EXCLUDED (see below).
#
# Deps already in place:
#   __mmap/__munmap/__mprotect — musl_glue.c (wrap sys_mmap/munmap/mprotect);
#     __tz.c needs __munmap, __map_file.c needs __mmap.
#   __sys_open — musl src/internal/syscall.h macro (→ __syscall(SYS_open,…));
#     __map_file.c uses it to open /etc/localtime (absent → no-op UTC).
#   getenv (musl_stdlib_objs), memcpy/strcpy/strlen (musl_string_objs),
#   __lock/__unlock + a_cas/a_crash/a_store/__wait/__wake (musl_pthread),
#   syscall_cp/__syscall_cp/__syscall/__syscall_ret (musl_pthread — already
#   proven by the unistd cancellable wrappers).
#   utimensat — musl_unistd_objs glob already builds src/unistd/utimensat.c;
#     musl utime.c calls it.
#   snprintf — musl_stdio_objs (stdio 全量迁移后接管；__asctime.c uses it).
#
# __nl_langinfo_l/nl_langinfo (src/locale/langinfo.c): __asctime.c/strftime.c/
# strptime.c pull weekday/month names via __nl_langinfo_l(CURRENT_LOCALE).
# CURRENT_LOCALE = __pthread_self()->locale, set to &libc.global_locale by
# __init_tp (musl_pthread) — same mechanism strerror already uses. With no
# locale data loaded, cat[]=NULL → returns the C-locale c_time[] table (Sun/
# Mon/.../Jan/.../AM/PM) — exactly the UTC English text we want. LCTRANS →
# __lctrans_impl weak pass-through (already in musl_pthread). langinfo.c is
# compiled here under musl-internal headers (the only locale source needed
# for the time tier); it also exports public nl_langinfo/nl_langinfo_l.
#
# __vdsosym: arch/x86_64/syscall_arch.h #defines VDSO_CGT_SYM, so musl's
# clock_gettime.c compiles the vdso probe path that calls __vdsosym. This OS
# maps no vdso; rather than compile src/internal/vdso.c (which walks
# libc.auxv for AT_SYSINFO_EHDR — risky if the loader's libc.auxv is unset),
# musl_glue.c provides a __vdsosym stub returning NULL: clock_gettime's first
# call runs cgt_init → __vdsosym=NULL → caches vdso_func=NULL → falls back
# to __syscall(SYS_clock_gettime). Identical to musl on a no-vdso kernel.
# vdso.c is therefore NOT compiled.
#
# sleep.c / usleep.c are NOT in src/time/ (they live in src/unistd/ and are
# handled by the unistd module's MUSL_UNISTD_EXCLUDE). musl's versions return
# the *remaining* interval on EINTR; this OS keeps the BSD "sleep the full
# duration" loop in user/lib/time.cc instead. That retention is a deliberate
# semantic choice, not a glob concern — file(GLOB src/time/*.c) never reaches
# src/unistd. Revisit (align to musl's non-looping semantics) is tracked in
# todo.md.
#
# timer_create.c / timer_settime.c / timer_delete.c / timer_gettime.c /
# timer_getoverrun.c EXCLUDED from the glob: kernel has no SYS_timer_*
# (return -ENOSYS) AND the SIGEV_THREAD path needs heavy pthread internals
# (pthread_barrier_*/__reset_tls/SIGTIMER/__libc_sigaction). 0 callers.
# Deferred to todo.md.
#
# timespec_to_tm (repo non-standard, 0 callers) is deleted from time.cc and
# libc.map; musl has no equivalent.
file(GLOB MUSL_TIME_SOURCES ${MUSL_DIR}/src/time/*.c)
list(REMOVE_ITEM MUSL_TIME_SOURCES
    # wcsftime.c lives in src/time/ but is owned by the wchar module
    # (musl_wchar_objs) — compiling it here too multi-defines
    # __wcsftime_l/wcsftime at link. Known upstream quirk: wcsftime.c declares
    # a 5-arg __strftime_fmt_1 while strftime.c defines 6-arg (..., int pad);
    # on x86-64 SysV %r9 (pad) is garbage. 0 in-tree callers. See todo.md.
    ${MUSL_DIR}/src/time/wcsftime.c
    ${MUSL_DIR}/src/time/timer_create.c
    ${MUSL_DIR}/src/time/timer_settime.c
    ${MUSL_DIR}/src/time/timer_delete.c
    ${MUSL_DIR}/src/time/timer_gettime.c
    ${MUSL_DIR}/src/time/timer_getoverrun.c)
list(APPEND MUSL_TIME_SOURCES
    # locale: __nl_langinfo_l/nl_langinfo/nl_langinfo_l provider (C-locale)
    # for __asctime.c/strftime.c/strptime.c. c_locale.c defines
    # __c_locale (referenced by the C_LOCALE macro __asctime.c passes to
    # __nl_langinfo_l); without it libc.so has an undefined __c_locale the
    # loader fails to relocate at runtime.
    ${MUSL_DIR}/src/locale/langinfo.c
    ${MUSL_DIR}/src/locale/c_locale.c)
add_musl_lib(musl_time_objs SOURCES ${MUSL_TIME_SOURCES}
    # _XOPEN_SOURCE=700 matches musl's own strict build: it suppresses the
    # auto-_BSD_SOURCE that features.h would otherwise enable (clang default
    # -std=gnu* does NOT define __STRICT_ANSI__), which would make <string.h>
    # declare the BSD index()/rindex() functions and clash with __tz.c's
    # static const unsigned char *index variable. _XOPEN700 keeps POSIX/XOPEN
    # declarations (nl_langinfo, strptime, ABDAY_*) live while dropping _BSD.
    FLAGS "-D_XOPEN_SOURCE=700")
