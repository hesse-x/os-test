# modules/unistd.cmake — musl unistd integration (unistd_worklist M0.2/M0.3).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib (raw add_library used here), USER_FREESTANDING_FLAGS.
# ===================== musl unistd integration (unistd_worklist M0.2/M0.3) =====================
# Build the upstream musl src/unistd/*.c into libc via a separate OBJECT library with a
# musl-internal include order. The musl sources #include "syscall.h" / "libc.h" (quoted),
# which must resolve to musl's OWN src/internal/{syscall,libc}.h — NOT this repo's
# user/include/syscall.h (the sys_* wrapper layer). Achieved by ordering musl src/internal
# ahead of user/include on this target's include path only; the rest of libc keeps the
# repo order (user/include first). <bits/syscall.h> (SYS_*/__NR_* numbers) is generated
# into user/include/bits/syscall.h (matching musl's Makefile sed product), shared by both.
#
# pthread-mechanism coupling: musl's cancellable wrappers (read/write/fsync/
# pause/...) call syscall_cp(...) → __syscall_cp, and the set*id wrappers call
# __setxid. The real musl implementations route through the pthread cancel/
# synccall machinery (src/thread/__syscall_cp.c, syscall_cp.s, synccall.c,
# setxid.c, pthread_cancel.c). Now that the repo uses musl's pthread
# (musl_pthread, see below), __syscall_cp comes from musl's REAL
# src/thread/__syscall_cp.c + x86_64/syscall_cp.s (cancel is live). __setxid
# still cannot use musl's setxid.c — it needs __synccall (procfs /proc/self/task,
# absent), and this kernel's creds are process-wide — so lib/musl_shim/syscall_cp.c
# keeps a no-broadcast __setxid (direct syscall), matching sys_process.cc.
#
# __syscall_ret (the -errno → -1+errno translation every musl wrapper's
# syscall(...) macro depends on) is provided by musl_pthread's
# src/internal/syscall_ret.c — the single source for both unistd and fcntl
# wrappers (and musl's own thread sources). It is NOT pulled into
# musl_unistd_objs to avoid a duplicate-definition clash with musl_pthread.
#
# faccessat.c is intentionally EXCLUDED this batch: its AT_EACCESS path needs
# __block_all_sigs/__restore_sigs/__clone shims; the repo's existing faccessat
# (file.cc) is retained for now (tracked in unistd_worklist problem 4).
#
# ttyname.c / ttyname_r.c are EXCLUDED: musl's ttyname_r readlinks /proc/self/fd/N
# (via __procfdname, src/internal/procfdname.c) and stats the result — this kernel
# has no procfs. The repo's existing ttyname (file.cc) uses ioctl(TIOCGPTN) instead
# and is retained. ttyname_r deficit recorded in doc/design/todo.md.
#
# isatty.c is EXCLUDED: musl probes TIOCGWINSZ, but this kernel's serial tty
# (kernel/driver/serial.c:211) only answers TCGETS and returns -ENOTTY for anything
# else — so musl's isatty would report the serial console (where the shell runs) as
# not-a-tty. The repo's isatty (file.cc) uses TCGETS, which both PTY and serial
# answer, and is retained.
#
# chdir.c / getcwd.c / renameat.c / unlinkat.c ARE ADOPTED: the kernel's *at
# syscalls resolve AT_FDCWD to the process cwd (resolve_dirfd_start in
# kernel/bsd/vfs.c resolves bp->cwd to its inode — the M0.4 fix), and the plain
# syscalls resolve relatives against bp->cwd via vfs_resolve_user. musl's
# chdir/getcwd make bp->cwd the sole source of truth (no userspace cwd_path
# copy), and musl's unlinkat/renameat pass AT_FDCWD straight through to the
# kernel, which honors chdir. The repo's file.cc openat/chdir/getcwd/unlinkat/
# renameat wrappers and the cwd_path/resolve_at_path helpers are deleted.

# musl unistd sources to adopt (pure wrappers + cancellable + set*id + library-logic).
# Built as an OBJECT library so its .o files merge into both libc.a and libc.so via
# add_user_lib(EXTRA_OBJS $<TARGET_OBJECTS:musl_unistd_objs>).
file(GLOB MUSL_UNISTD_SOURCES ${MUSL_DIR}/src/unistd/*.c)
# Exclude sources we are not adopting this batch (see comments above / below).
set(MUSL_UNISTD_EXCLUDE
    ${MUSL_DIR}/src/unistd/faccessat.c   # AT_EACCESS clone path — repo faccessat retained
    ${MUSL_DIR}/src/unistd/setxid.c      # __setxid provided by musl_shim/syscall_cp.c
    ${MUSL_DIR}/src/unistd/ttyname.c     # wraps ttyname_r (excluded below)
    ${MUSL_DIR}/src/unistd/ttyname_r.c   # needs /proc/self/fd (no procfs) — repo ttyname retained
    ${MUSL_DIR}/src/unistd/isatty.c      # musl probes TIOCGWINSZ; serial only answers TCGETS — repo isatty retained
    ${MUSL_DIR}/src/unistd/gethostname.c # musl returns uname.nodename ("(none)"); repo reads sys_gethostname (sethostname round-trip) — retained
    ${MUSL_DIR}/src/unistd/sleep.c       # repo sleep resumes after EINTR (musl returns remaining); retained for signal-resume semantics
    ${MUSL_DIR}/src/unistd/usleep.c      # repo usleep resumes after EINTR (musl returns early); retained (same reason as sleep)
)
list(REMOVE_ITEM MUSL_UNISTD_SOURCES ${MUSL_UNISTD_EXCLUDE})

add_library(musl_unistd_objs OBJECT
    ${MUSL_UNISTD_SOURCES}
    ${MUSL_DIR}/src/internal/procfdname.c
    ${MUSL_DIR}/src/exit/_Exit.c
    ${MUSL_DIR}/src/sched/sched_yield.c
    # nice.c (in src/unistd) calls setpriority/getpriority, whose impls live in
    # src/misc — pull them in or libc.so has an unresolved PLT 'setpriority'.
    ${MUSL_DIR}/src/misc/setpriority.c
    ${MUSL_DIR}/src/misc/getpriority.c
    # alarm.c / ualarm.c (in src/unistd) call setitimer, impl in src/signal.
    ${MUSL_DIR}/src/signal/setitimer.c
    # rename.c lives in src/stdio (not src/unistd); adopt musl's (routes to
    # SYS_rename, resolved cwd-relative via vfs_resolve_user) so the repo's
    # file.cc rename wrapper — which lacked LIBC_EXPORT and was hidden in
    # libc.so despite being in libc.map — is dropped.
    ${MUSL_DIR}/src/stdio/rename.c
    ${CMAKE_SOURCE_DIR}/user/lib/musl_shim/syscall_cp.c
)
# musl-internal include order: musl src/internal BEFORE user/include so the musl
# sources' quoted #include "syscall.h"/"libc.h" resolve to musl's own headers.
# arch/x86_64 provides syscall_arch.h (inline __syscallN stubs); arch/generic is the
# bits fallback. user/include carries the generated bits/syscall.h + xos headers.
# Relaxed warnings (-Wno-all): upstream musl is third-party code, not under our
# -Werror gate (same rationale as add_drm_lib for third_party/drm).
target_include_directories(musl_unistd_objs PRIVATE
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi
)
# -Wno-visibility: like -Wempty-body, -Wvisibility is NOT grouped under -Wall
# (clang keeps it outside -Wno-all). musl's <termios.h> declares
# tcsetwinsize(int, const struct winsize *) right after __NEED_struct_winsize,
# before the full struct definition is in scope, so tcsetpgrp.c/tcgetpgrp.c
# (which #include <termios.h>) trip -Wvisibility. Third-party source — silence.
target_compile_options(musl_unistd_objs PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fno-pie -Wno-all -Wno-visibility)

# libc.so needs PIC objects (the -fno-pie objects above produce non-PIC
# relocations like R_X86_64_32 against .rodata, which ld rejects when building a
# shared object). Mirror the repo's libc.a(-fno-pie)/libc.so(-fPIC) dual build:
# a second OBJECT library, musl_unistd_objs_so, compiles the SAME musl sources
# with -fPIC for the shared link. Same sources, same include order, same shim —
# only the code model differs.
add_library(musl_unistd_objs_so OBJECT
    ${MUSL_UNISTD_SOURCES}
    ${MUSL_DIR}/src/internal/procfdname.c
    ${MUSL_DIR}/src/exit/_Exit.c
    ${MUSL_DIR}/src/sched/sched_yield.c
    ${MUSL_DIR}/src/misc/setpriority.c
    ${MUSL_DIR}/src/misc/getpriority.c
    ${MUSL_DIR}/src/signal/setitimer.c
    ${MUSL_DIR}/src/stdio/rename.c
    ${CMAKE_SOURCE_DIR}/user/lib/musl_shim/syscall_cp.c
)
target_include_directories(musl_unistd_objs_so PRIVATE
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi
)
target_compile_options(musl_unistd_objs_so PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fPIC -Wno-all -Wno-visibility)
# The SHARED libc.so link consumes the PIC objects as bare .o files; the STATIC
# libc.a path consumes the -fno-pie objects via $<TARGET_OBJECTS:...>. Both are
# wired through EXTRA_OBJS below.
