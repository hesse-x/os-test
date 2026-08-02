# modules/unistd.cmake — musl unistd integration (unistd_worklist M0.2/M0.3).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib (raw add_library used here), USER_FREESTANDING_FLAGS.
# ===================== musl unistd integration (unistd_worklist M0.2/M0.3) =====================
# Build the upstream musl src/unistd/*.c into libc via a separate OBJECT library with a
# musl-internal include order. The musl sources #include "syscall.h" / "libc.h" (quoted),
# which must resolve to musl's OWN src/internal/{syscall,libc}.h — NOT this repo's
# user/include/xos/syscall_ext.h (the sys_* wrapper layer). Achieved by ordering musl src/internal
# ahead of user/include on this target's include path only. <bits/syscall.h>
# (SYS_*/__NR_* numbers) is generated into build/musl_gen, matching musl's
# Makefile output.
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
# faccessat.c is ADOPTED: its AT_EACCESS clone path needs
# __block_all_sigs/__restore_sigs (src/signal/block.c, compiled by musl_pthread)
# + __clone (src/thread/clone.c, musl_pthread); __sys_wait4 is just an
# __syscall(SYS_wait4,...) macro (no standalone symbol), and the kernel's
# SYS_WAIT4 accepts __WCLONE. The clone child's setregid/setreuid/getegid/
# geteuid/faccessat syscalls are all kernel-implemented. The repo's faccessat
# (file.cc) is deleted to avoid a duplicate definition.
#
# ttyname.c / ttyname_r.c ARE ADOPTED: procfs now provides /proc/self/fd/N
# (procfs M4), so musl's ttyname_r readlinks it and stats the result for
# dev+ino cross-check (the Linux-canonical path). The repo's old ttyname
# (file.cc, ioctl TIOCGPTN) is deleted to avoid a duplicate definition.
# musl's ttyname_r calls isatty() — the repo's isatty (file.cc, TCGETS) is
# retained (musl's isatty.c still excluded — it probes TIOCGWINSZ which the
# serial tty doesn't answer).
#
# isatty.c is EXCLUDED: musl probes TIOCGWINSZ, but this kernel's serial tty
# (kernel/driver/serial.c:211) only answers TCGETS and returns -ENOTTY for anything
# else — so musl's isatty would report the serial console (where the shell runs) as
# not-a-tty. The repo's isatty (file.cc) uses TCGETS, which both PTY and serial
# answer, and is retained.
#
# sleep.c / usleep.c ARE ADOPTED: musl returns the *remaining* interval on EINTR
# (the POSIX/SUS behavior); the repo's old time.cc wrappers looped to resume the
# remainder (BSD sleep(3) "sleep the full duration"). No caller in-tree depends on
# the return value (all use bare `sleep(n);`/`usleep(n);`), and init/terminal
# crash-restart backoff tolerates early return (the waitpid retry loop re-scans).
# musl's sleep.c/usleep.c layer on the already-built nanosleep (time.cmake). The
# repo's time.cc is deleted (it held only sleep/usleep).
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
file(GLOB MUSL_SCHED_SOURCES CONFIGURE_DEPENDS ${MUSL_DIR}/src/sched/*.c)
# Exclude sources we are not adopting this batch (see comments above / below).
set(MUSL_UNISTD_EXCLUDE
    ${MUSL_DIR}/src/unistd/setxid.c      # __setxid provided by musl_shim/syscall_cp.c
    ${MUSL_DIR}/src/unistd/isatty.c      # musl probes TIOCGWINSZ; serial only answers TCGETS — repo isatty retained
    ${MUSL_DIR}/src/unistd/gethostname.c # musl returns uname.nodename ("(none)"); repo reads sys_gethostname (sethostname round-trip) — retained
)
list(REMOVE_ITEM MUSL_UNISTD_SOURCES ${MUSL_UNISTD_EXCLUDE})

add_library(musl_unistd_objs OBJECT
    ${MUSL_UNISTD_SOURCES}
    ${MUSL_SCHED_SOURCES}
    ${MUSL_DIR}/src/internal/procfdname.c
    ${MUSL_DIR}/src/exit/_Exit.c
    # nice.c (in src/unistd) calls setpriority/getpriority, whose impls live in
    # src/misc — pull them in or libc.so has an unresolved PLT 'setpriority'.
    ${MUSL_DIR}/src/misc/setpriority.c
    ${MUSL_DIR}/src/misc/getpriority.c
    # src/misc/syscall.c — the public indirect-syscall primitive syscall(number,
    # ...) (va_arg wrapper over arch __syscallN + __syscall_ret). Cross-built C++
    # runtimes need it: libc++abi's __cxa_guard_acquire uses syscall(SYS_gettid)
    # and syscall(SYS_futex, ...) for thread-safe static-init, and libc++'s
    # <atomic> wait/wake does the same. No module compiled it before, so libc.so
    # had no `syscall` symbol — exported via libc.map's <sys/syscall.h> block.
    ${MUSL_DIR}/src/misc/syscall.c
    # alarm.c / ualarm.c (in src/unistd) call setitimer, impl in src/signal.
    ${MUSL_DIR}/src/signal/setitimer.c
    # rename.c lives in src/stdio (not src/unistd); adopt musl's (routes to
    # SYS_rename, resolved cwd-relative via vfs_resolve_user) so the repo's
    # file.cc rename wrapper — which lacked LIBC_EXPORT and was hidden in
    # libc.so despite being in libc.map — is dropped.
    ${MUSL_DIR}/src/stdio/rename.c
    # sendfile.c lives in src/linux (not src/unistd); adopt musl's (thin
    # SYS_sendfile wrapper). The kernel already implements sys_sendfile
    # (kernel/bsd/syscall.c SYS_SENDFILE), but libc had neither the symbol nor
    # the declaration, so libc++'s filesystem copy_file — which gates on
    # __has_include(<sys/sendfile.h>) — picked up musl's <sys/sendfile.h>
    # (published to the sysroot) and compiled the sendfile path with no symbol
    # to link against → undefined reference. sendfile.c's only deps are
    # <sys/sendfile.h> (in sysroot) + "syscall.h" (musl-internal, on this
    # target's include path) — same as the unistd wrappers.
    ${MUSL_DIR}/src/linux/sendfile.c
    # umask lives in src/stat (not src/unistd), so the unistd glob misses it and
    # no module globs src/stat. ADOPTED here as a one-line SYS_umask wrapper
    # (kernel implements SYS_UMASK); the repo's unistd.cc wrapper — and thus the
    # whole now-empty unistd.cc — is deleted.
    ${MUSL_DIR}/src/stat/umask.c
    ${CMAKE_SOURCE_DIR}/user/lib/musl_shim/syscall_cp.c
)
# musl-internal include order: musl src/internal BEFORE user/include so the musl
# sources' quoted #include "syscall.h"/"libc.h" resolve to musl's own headers.
# arch/x86_64 provides syscall_arch.h (inline __syscallN stubs); arch/generic is the
# bits fallback. user/include carries the generated bits/syscall.h + xos headers.
# Relaxed warnings (-Wno-all): upstream musl is third-party code, not under our
# -Werror gate (same rationale as add_drm_lib for third_party/drm).
target_include_directories(musl_unistd_objs PRIVATE
    ${MUSL_GEN_INCLUDE_DIR}
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi
)
add_dependencies(musl_unistd_objs musl_headers)
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
    ${MUSL_SCHED_SOURCES}
    ${MUSL_DIR}/src/internal/procfdname.c
    ${MUSL_DIR}/src/exit/_Exit.c
    ${MUSL_DIR}/src/misc/setpriority.c
    ${MUSL_DIR}/src/misc/getpriority.c
    ${MUSL_DIR}/src/misc/syscall.c
    ${MUSL_DIR}/src/signal/setitimer.c
    ${MUSL_DIR}/src/stdio/rename.c
    # sendfile.c lives in src/linux (not src/unistd); adopt musl's (thin
    # SYS_sendfile wrapper). The kernel already implements sys_sendfile
    # (kernel/bsd/syscall.c SYS_SENDFILE), but libc had neither the symbol nor
    # the declaration, so libc++'s filesystem copy_file — which gates on
    # __has_include(<sys/sendfile.h>) — picked up musl's <sys/sendfile.h>
    # (published to the sysroot) and compiled the sendfile path with no symbol
    # to link against → undefined reference. sendfile.c's only deps are
    # <sys/sendfile.h> (in sysroot) + "syscall.h" (musl-internal, on this
    # target's include path) — same as the unistd wrappers.
    ${MUSL_DIR}/src/linux/sendfile.c
    # umask — see the -fno-pie list above (src/stat, one-line SYS_umask wrapper).
    ${MUSL_DIR}/src/stat/umask.c
    ${CMAKE_SOURCE_DIR}/user/lib/musl_shim/syscall_cp.c
)
target_include_directories(musl_unistd_objs_so PRIVATE
    ${MUSL_GEN_INCLUDE_DIR}
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi
)
add_dependencies(musl_unistd_objs_so musl_headers)
target_compile_options(musl_unistd_objs_so PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fPIC -Wno-all -Wno-visibility)
# The SHARED libc.so link consumes the PIC objects as bare .o files; the STATIC
# libc.a path consumes the -fno-pie objects via $<TARGET_OBJECTS:...>. Both are
# wired through EXTRA_OBJS below.
