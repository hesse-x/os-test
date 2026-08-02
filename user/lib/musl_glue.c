/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * musl pthread glue — hidden internal aliases musl pthread calls.
 *
 * musl's pthread is compiled verbatim from third_party/musl (musl_pthread
 * sub-library), but it does not stand alone: the thread code calls a set of
 * *hidden* internal symbols (names not exposed in musl's public headers)
 * that normally live in musl's libc. We keep our own libc (printf/malloc/
 * string/FILE/...) and only replace pthread, so those hidden symbols must be
 * provided here as thin forwarders to our existing syscall wrappers.
 *
 * Hidden (visibility("hidden")) so they satisfy musl's internal references
 * without polluting the exported libc ABI.
 *
 * sigaction is NO LONGER glued here. The kernel's sys_sigaction wire ABI now
 * matches musl's struct k_sigaction (arch/x86_64/ksigaction.h: handler/flags/
 * restorer/mask[2], 40 bytes) and accepts any sigsetsize >= 8, so musl's
 * upstream src/signal/sigaction.c is compiled directly (signal.cmake) and its
 * __libc_sigaction/__get_handler_set are the real backend. musl's
 * rt_sigprocmask path (block.c) likewise passes arg4 = _NSIG/8 = 16 with a
 * 128-byte buffer, which the kernel handles by reading only the low 8 bytes.
 */

/* musl internal TCB / __libc touchpoints are reached via these forwarders only;
 * we do NOT include musl private headers here (kept free-standing on our libc
 * side). pthread_impl.h would pull musl's struct pthread layout into libc.a,
 * which we deliberately avoid. */

/* Musl does not define a `hidden` attribute macro; its internal symbols get
 * hidden visibility via ATTR_LIBC_VISIBILITY / version scripts. Our glue lives
 * in libc.a/libc.so, so mark the forwarders hidden to keep them out of the
 * exported libc ABI (musl_pthread.a resolves them by plain global name). */
#define MUSL_HIDDEN __attribute__((visibility("hidden")))

/* __mmap/__munmap/__mprotect/__mremap are NO LONGER provided here. They were
 * hidden forwarders to sys_mmap/sys_munmap/sys_mprotect that existed only
 * because the repo's own libc had no real mmap/munmap/mprotect/mremap objects
 * for musl's pthread/malloc/locale/time to link against. The mman module has
 * switched to musl upstream (musl_mman_objs): musl's src/mman/{mmap,munmap,
 * mprotect,mremap}.c now supply the real __mmap/__munmap/__mprotect/__mremap
 * (weak_alias to mmap/munmap/mprotect/mremap). Keeping the forwarders here
 * would multi-define those hidden symbols at link. musl's versions route
 * through the same SYS_mmap(9)/SYS_munmap(11)/SYS_mprotect(10)/SYS_mremap(25)
 * the forwarders used, so behaviour is unchanged (musl's mmap.c additionally
 * fixes an EPERM→ENOMEM and validates offset alignment — an improvement). */

/* __vdsosym — musl's clock_gettime.c probes for a vdso clock_gettime via
 * __vdsosym (compiled in because arch/x86_64/syscall_arch.h #defines
 * VDSO_CGT_SYM). This OS maps no vdso; return NULL so clock_gettime's first
 * call caches vdso_func=NULL (via cgt_init) and falls back to
 * __syscall(SYS_clock_gettime) — identical to musl on a no-vdso kernel.
 * musl's src/internal/vdso.c is NOT compiled (it would walk libc.auxv for
 * AT_SYSINFO_EHDR, unnecessary and risky if the loader leaves libc.auxv
 * unset). __clock_gettime itself now comes from musl's clock_gettime.c
 * (musl_time_objs), so the old __clock_gettime forwarder is retired. */
MUSL_HIDDEN void *__vdsosym(const char *vername, const char *name) {
  (void)vername;
  (void)name;
  return 0;
}

/* __fstat — musl's src/time/__map_file.c (mapped-file reader for TZ/locale
 * data: __tz.c reads /etc/localtime, locale_map.c/catopen read locale files)
 * calls the hidden __fstat to get the size of an already-open fd before
 * __mmap'ing it. This OS has no /etc/localtime and no locale data, so
 * __map_file's callers short-circuit (NULL path / open fails) and __fstat is
 * effectively dead at runtime — but it is still a link-time reference that
 * must resolve or libc.so fails with "undefined hidden symbol `__fstat`".
 *
 * We do NOT compile musl's src/stat/fstat.c here: that drags in fstatat.c's
 * statx→stat conversion (a whole stat module), and would multi-define the
 * public fstat/stat/fstatat that this repo's user/lib/file.cc already
 * provides via the same statx AT_EMPTY_PATH path the kernel implements
 * (SYS_STATX=332; SYS_fstatat/SYS_lstat are absent). Instead, mirror musl's
 * own fstat.c trick: provide the hidden __fstat and let it forward to the
 * public fstat (defined in file.cc), so both the hidden musl reference and
 * the public libc surface resolve to the same code. See malloc.md / the
 * time-module note for the broader "hidden-symbol glue" pattern. */
struct stat; /* forward — full prototype is in <sys/stat.h> (public header) */
MUSL_HIDDEN int __fstat(int fd, struct stat *st) {
  extern int fstat(int fd, struct stat *st);
  return fstat(fd, st);
}
