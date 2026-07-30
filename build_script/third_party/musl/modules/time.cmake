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
#   snprintf — user/lib/stdio.cc (__asctime.c uses it).
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
# sleep.c / usleep.c are EXCLUDED from this list: the repo's sleep/usleep
# (kept in the slimmed user/lib/time.cc) loop on EINTR to sleep the FULL
# duration, whereas musl's return the remaining interval. This deliberate
# semantic choice was made in the unistd migration (MUSL_UNISTD_EXCLUDE);
# retained here. They call the now-musl nanosleep().
#
# wcsftime.c now compiled in musl_wchar_objs (wchar tier landed); its time deps
# (the strftime machinery below) resolve via the merged libc.a/libc.so. Known
# upstream bug: wcsftime.c declares/calls 5-arg __strftime_fmt_1 while strftime.c
# defines 6-arg (..., int pad) — on x86-64 SysV ABI %r9 (pad) is garbage. 0
# in-tree callers; compiled verbatim (matches musl upstream). See todo.md.
#
# timer_create.c / timer_settime.c / timer_delete.c / timer_gettime.c /
# timer_getoverrun.c EXCLUDED: kernel has no SYS_timer_* (return -ENOSYS) AND
# the SIGEV_THREAD path needs heavy pthread internals (pthread_barrier_*/
# __reset_tls/SIGTIMER/__libc_sigaction). 0 callers. Deferred to todo.md.
#
# timespec_to_tm (repo non-standard, 0 callers) is deleted from time.cc and
# libc.map; musl has no equivalent.
set(MUSL_TIME_SOURCES
    # pure compute
    ${MUSL_DIR}/src/time/difftime.c
    # calendar conversion helpers (used by gmtime_r/mktime/timegm/strftime)
    ${MUSL_DIR}/src/time/__year_to_secs.c
    ${MUSL_DIR}/src/time/__month_to_secs.c
    ${MUSL_DIR}/src/time/__tm_to_secs.c
    ${MUSL_DIR}/src/time/__secs_to_tm.c
    # timezone machinery: __secs_to_zone (localtime/mktime), __utc (gmtime_r/
    # timegm), tzset/timezone/daylight/tzname. Reads TZ env + /etc/localtime
    # (absent → defaults UTC, matching the old repo UTC-only stub).
    ${MUSL_DIR}/src/time/__tz.c
    ${MUSL_DIR}/src/time/__map_file.c
    # calendar wrappers
    ${MUSL_DIR}/src/time/gmtime.c
    ${MUSL_DIR}/src/time/gmtime_r.c
    ${MUSL_DIR}/src/time/localtime.c
    ${MUSL_DIR}/src/time/localtime_r.c
    ${MUSL_DIR}/src/time/mktime.c
    ${MUSL_DIR}/src/time/timegm.c
    # asctime/ctime (deps: snprintf + __nl_langinfo_l via __asctime.c)
    ${MUSL_DIR}/src/time/asctime.c
    ${MUSL_DIR}/src/time/asctime_r.c
    ${MUSL_DIR}/src/time/__asctime.c
    ${MUSL_DIR}/src/time/ctime.c
    ${MUSL_DIR}/src/time/ctime_r.c
    # formatting (deps: __nl_langinfo_l, __tm_to_secs)
    ${MUSL_DIR}/src/time/strftime.c
    ${MUSL_DIR}/src/time/strptime.c
    # syscall-backed clock wrappers
    ${MUSL_DIR}/src/time/clock.c
    ${MUSL_DIR}/src/time/clock_gettime.c
    ${MUSL_DIR}/src/time/clock_settime.c
    ${MUSL_DIR}/src/time/clock_getres.c
    ${MUSL_DIR}/src/time/clock_getcpuclockid.c
    ${MUSL_DIR}/src/time/clock_nanosleep.c
    ${MUSL_DIR}/src/time/nanosleep.c
    ${MUSL_DIR}/src/time/gettimeofday.c
    ${MUSL_DIR}/src/time/time.c
    ${MUSL_DIR}/src/time/timespec_get.c
    # legacy / misc (0 callers, trivial compile-through; deps already in place)
    ${MUSL_DIR}/src/time/times.c
    ${MUSL_DIR}/src/time/utime.c
    ${MUSL_DIR}/src/time/ftime.c
    ${MUSL_DIR}/src/time/getdate.c
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
