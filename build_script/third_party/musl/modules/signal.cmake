# modules/signal.cmake — musl signal integration (musl_worklist).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl signal integration =====================
# Build the upstream musl signal sources that are SAFE for this OS into libc,
# alongside the repo's hand-written user/lib/signal.cc adapter layer (which
# STAYS — see below). The <signal.h> header is already musl's (no repo
# user/include/signal.h; install-headers.sh §3c publishes musl's signal.h +
# bits/signal.h).
#
# Why signal.cc is NOT deleted (unlike ctype/string/stdio): the 8 core signal
# syscalls cross a wire-ABI boundary. The kernel's sys_sigaction reads a
# 32-byte struct sigaction with an 8-byte sigset_t mask and REJECTS the call
# unless the mask-size argument == 8. musl's own src/signal/sigaction.c builds
# a 40-byte k_sigaction and passes _NSIG/8 = 16, which the kernel rejects, so
# musl_glue.c::__libc_sigaction converts musl's 152-byte user struct to the
# 32-byte kernel wire struct (passing literal 8) and is the real sigaction
# backend. signal.cc wraps kill/raise/signal/sigaction/sigprocmask/sigpending/
# sigaltstack/sigreturn around that glue. Therefore this module compiles ONLY
# musl signal sources that are pure-userspace or thin-syscall AND not already
# defined in signal.cc (no multi-define) AND not touching the wire sigaction
# struct.
#
# SAFE set compiled here (none defined in signal.cc, none multi-define):
#   killpg        — pure userspace: kill(-pgid, sig) after an EINVAL guard.
#   sigorset/sigandset/sigisemptyset — pure sigset_t bitwise ops (_GNU_SOURCE).
#   sighold/sigrelse — sigemptyset+sigaddset+sigprocmask (all already exported).
#   sigset/sigignore/siginterrupt — call the public sigaction() (routes to
#                   musl_glue::__libc_sigaction, link+runtime OK) + sigprocmask.
#   siglongjmp    — calls longjmp (already exported); mask restore is deferred
#                   to sigsetjmp's return context (musl design).
#   getitimer     — syscall(SYS_getitimer); SYS_GETITIMER=36 IS dispatched.
#   psignal/psiginfo — fprintf(stderr, "%s: %s", msg, strsignal(sig)); pure
#                   userspace over musl stdio (already migrated).
#   strsignal.c (from src/string/, NOT src/signal/) — psignal's dependency.
#                   It is NOT built by musl_string_objs (string.cmake excludes
#                   it: 0 callers there), so this module is its single owner —
#                   no multi-define clash. Routes strings through LCTRANS_CUR →
#                   __lctrans_cur (musl __lctrans.c, in musl_pthread).
#
# EXCLUDED (deliberately):
#   sigaction/raise/signal/kill/sigprocmask/sigpending/sigaltstack — already
#     defined in signal.cc (multi-define).
#   sigtimedwait/sigqueue/sigsuspend/sigpause — the kernel has no
#     SYS_rt_sigtimedwait / SYS_rt_sigqueueinfo / SYS_rt_sigsuspend, so these
#     return -ENOSYS at runtime. sigwait/sigwaitinfo are dropped too because
#     they call sigtimedwait (would be an undefined reference without it);
#     the whole sigwait family is deferred until the kernel implements the
#     rt_sigtimedwait/queueinfo/suspend syscalls (doc/design/todo.md).
#   setitimer — already built in musl_unistd_objs (alarm/ualarm use it).
#   block.c / sigrtmin.c / sigrtmax.c / sigemptyset/fillset/addset/delset/
#     ismember — already built in musl_pthread.
#   sigsetjmp.c — empty stub (the real sigsetjmp is src/setjmp/x86_64/ asm).
add_musl_lib(musl_signal_objs SOURCES
    ${MUSL_DIR}/src/signal/killpg.c
    ${MUSL_DIR}/src/signal/sigorset.c
    ${MUSL_DIR}/src/signal/sigandset.c
    ${MUSL_DIR}/src/signal/sigisemptyset.c
    ${MUSL_DIR}/src/signal/sighold.c
    ${MUSL_DIR}/src/signal/sigrelse.c
    ${MUSL_DIR}/src/signal/sigset.c
    ${MUSL_DIR}/src/signal/sigignore.c
    ${MUSL_DIR}/src/signal/siginterrupt.c
    ${MUSL_DIR}/src/signal/siglongjmp.c
    ${MUSL_DIR}/src/signal/getitimer.c
    ${MUSL_DIR}/src/signal/psignal.c
    ${MUSL_DIR}/src/signal/psiginfo.c
    # strsignal lives under src/string/ but is excluded from musl_string_objs
    # (0 callers there); this module is its single build owner — psignal needs it.
    ${MUSL_DIR}/src/string/strsignal.c)
