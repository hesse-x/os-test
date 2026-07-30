# modules/pthread.cmake — musl pthread integration (pthread.md §八).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: add_musl_lib. (musl_generate_headers() runs once at the top of
# musl_rules.cmake, before this file is included.)
# ===================== musl pthread integration (pthread.md §八) =====================
# musl pthread sub-library — compiled from musl upstream with musl-internal
# include paths (see add_musl_lib in build_script/cmake/user_rules.cmake).
# Provides: full pthread API (src/thread/*), TLS startup (__init_tls/__init_tp/
# __copy_tls), the struct __libc __libc global, __errno_location, block/restore
# signal internals, stack-canary init, sigaction restorer (__restore_rt), AND
# the sigset_t set-mutation functions (sigemptyset/fillset/addset/delset/
# ismember) — these operate on musl's 128-byte sigset_t and must be compiled
# under musl's internal headers, not our libc's. synccall.c excluded (needs
# procfs /proc/self/task; only reached by multithreaded setxid/setrlimit).
# Public symbols (malloc/memcpy/exit/...) stay in our libc; musl pthread
# reaches them via the hidden aliases in musl_glue.c.
file(GLOB MUSL_THREAD_SOURCES ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/*.c)
# Drop C sources that have an arch asm equivalent (src/thread/x86_64/<name>.s).
# On x86-64 musl uses the .s versions (clone/__set_thread_area/__unmapself/
# syscall_cp); the .c versions are fallbacks for archs without asm. Compiling
# both would duplicate the symbols at link, and __unmapself.c additionally
# pulls dynlink.h whose `typedef _Noreturn void (*stage3_func)` clang rejects.
# The .s versions are added explicitly below.
list(REMOVE_ITEM MUSL_THREAD_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/synccall.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/__unmapself.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/clone.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/__set_thread_area.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/syscall_cp.c)
set(MUSL_PTHREAD_SOURCES
    ${MUSL_THREAD_SOURCES}
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/x86_64/clone.s
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/x86_64/__set_thread_area.s
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/x86_64/__unmapself.s
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/thread/x86_64/syscall_cp.s
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/env/__init_tls.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/env/__stack_chk_fail.c
    # musl stdio symbols referenced by pthread_create's single-thread→threaded
    # transition (src/thread/pthread_create.c:200-210): it walks the open-FILE
    # list (__ofl_lock/__ofl_unlock, src/stdio/ofl.c) and re-locks
    # __stdin_used/__stdout_used/__stderr_used. Now that stdio is musl upstream
    # (musl_stdio_objs, stdio.md), the ofl list IS real — musl __fdopen/fopen
    # call __ofl_add to register each FILE, and the three *_used symbols are
    # STRONG-defined by musl stdin.c/stdout.c/stderr.c (no longer the NULL-dummy
    # weak aliases pthread_create.c falls back to). So init_file_lock walks a
    # populated list and the ofl_lock/unlock here are live, not just PROVIDE.
    # ofl.c + __lockfile.c stay compiled HERE (not in musl_stdio_objs) to keep a
    # single owner — musl_stdio_objs EXCLUDES them (see the musl stdio block) to
    # avoid a multi-define clash; their deps (__lock/a_cas/__wait/a_store/
    # __wake/__pthread_self) are all in musl_pthread already.
    # src/stdio/__stdio_exit.c is NOT here either: it is now built via the
    # musl_stdio_objs glob (it walks the ofl list + __stdin_used/__stdout_used to
    # flush on exit), and the repo's strong user/lib/stdio_exit.c override is
    # deleted (it flushed the repo's non-musl stdout/stderr, which no longer
    # exist). exit is musl's src/exit/exit.c (musl_stdlib_objs) → __stdio_exit.
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/stdio/ofl.c
    # __lockfile/__unlockfile: the FLOCK/FUNLOCK/FFINALLOCK primitives every
    # locked musl stdio function expands (fopen/fclose/fread/fwrite/...). Now
    # genuinely reachable via musl stdio paths (no longer "only via __stdio_exit
    # which we replace"). Deps (__pthread_self, a_cas, __wait, a_store, __wake)
    # are all in musl_pthread; operates on musl FILE->lock/waiters.
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/stdio/__lockfile.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/signal/block.c
    # sigset_t set-mutation helpers (musl's 128-byte sigset_t). Compiled here
    # under musl-internal headers, then merged into libc so every consumer
    # (libc.so's abort()/sigprocmask path, and any program) gets them.
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/signal/sigemptyset.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/signal/sigfillset.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/signal/sigaddset.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/signal/sigdelset.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/signal/sigismember.c
    # SIGRTMIN/SIGRTMAX macros in musl <signal.h> expand to
    # __libc_current_sigrtmin()/__libc_current_sigrtmax(); without these two
    # sources those are undefined references. They stay hidden (internal) — the
    # macros are the public surface.
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/signal/sigrtmin.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/signal/sigrtmax.c
    # restore.s provides __restore_rt (mov $15; syscall = rt_sigreturn), used by
    # musl_glue.c's __libc_sigaction as the kernel sa_restorer so rt_sigreturn
    # returns correctly after a musl-installed handler (the pthread_cancel
    # handler in particular).
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/signal/x86_64/restore.s
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/internal/libc.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/errno/__errno_location.c
    # strerror.c builds errid[]/errmsg[] from __strerror.h (double-inclusion
    # macro table) and provides strerror/__strerror_l/strerror_l, replacing the
    # hand-written switch in user/lib/string.cc so every errno maps to the
    # canonical Linux/glibc message. It routes messages through LCTRANS/
    # CURRENT_LOCALE (src/internal/locale_impl.h): CURRENT_LOCALE =
    # __pthread_self()->locale, set to &libc.global_locale by __init_tp
    # (__init_tls.c:19, libc.c already above) — non-null at runtime. LCTRANS →
    # __lctrans(msg, loc->cat[LC_MESSAGES]) comes from __lctrans.c, whose
    # weak_alias(dummy, __lctrans_impl) pass-through returns msg unchanged
    # (we deliberately do NOT compile locale_map.c's strong __lctrans_impl —
    # no gettext/catalog in this OS, so C-locale English text wins, exactly
    # like a no-i18n musl build). __lctrans.c is strerror.c's only locale dep;
    # no other locale sources are pulled in. The mips EDQUOT==1133 remap guard
    # in strerror.c is dead on x86-64 (EDQUOT==122), compiled to nothing.
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/errno/strerror.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/locale/__lctrans.c
    # __syscall (asm) + __syscall_ret (C): the two low-level syscall primitives
    # musl's thread code calls (src/thread/__futex.c → syscall(SYS_futex);
    # src/thread/__syscall_cp.c → __syscall; many src/env/*.c → __syscall_ret).
    # Declarations live in src/internal/syscall.h:25 but their definitions are
    # NOT in the src/thread/*.c glob: __syscall is per-arch asm
    # (src/internal/x86_64/syscall.s — loads %rax=n, shuffles args to the Linux
    # x86-64 syscall register order rdi/rsi/rdx/r10/r8/r9, `syscall`), and
    # __syscall_ret is src/internal/syscall_ret.c (errno translation for values
    # > -4096). Our kernel dispatch uses the identical Linux x86-64 numbers
    # (verified: futex=202/clone=56/tkill=200/rt_sigaction=13/...), so musl's
    # __syscall reaches our kernel with no glue. Both must be compiled here so
    # the merged libc.a/libc.so define them; otherwise the link fails with
    # "hidden symbol `__syscall`/`__syscall_ret` isn't defined".
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/internal/x86_64/syscall.s
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/internal/syscall_ret.c
)
add_musl_lib(musl_pthread SOURCES ${MUSL_PTHREAD_SOURCES})
