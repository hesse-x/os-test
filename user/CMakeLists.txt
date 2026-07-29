# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/ — userspace libc + test programs

# libc source list (dual artifacts from same source: libc.a + libc.so)
# start.cc removed (_start moved to crt0.o, see plan_ld2b3 T4/T21)
# Added start_main.cc (__libc_start_main, T11), atexit.c (T8), init_array.c (T10)
# pthread.cc/tls.cc/errno.cc removed: pthread now comes from musl (musl_pthread
# sub-library below). __errno_location is provided by musl's
# src/errno/__errno_location.c (reads musl struct pthread at %fs:0). musl_glue.c
# provides the hidden internal aliases musl pthread calls.
set(LIBC_SOURCES
    lib/stdio.cc lib/string.cc lib/malloc.cc
    lib/unistd.cc lib/time.cc lib/file.cc
    lib/ctype.c lib/strtol.c lib/stdlib_misc.c lib/uname.c
    lib/assert.c lib/signal.cc lib/getrandom.c
    lib/musl_missing.c
    lib/musl_glue.c
    lib/setjmp.S
    lib/start_main.cc
    lib/io_multiplex.cc
    lib/sys_process.cc
    lib/sys_device.cc
    lib/sys_ipc.cc
    lib/sys_socket.cc
    lib/musl_loader_shim.c
    ${CMAKE_SOURCE_DIR}/utils/kvformat.c
)

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
set(MUSL_DIR ${CMAKE_SOURCE_DIR}/third_party/musl)

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
    lib/musl_shim/syscall_cp.c
)
# musl-internal include order: musl src/internal BEFORE user/include so the musl
# sources' quoted #include "syscall.h"/"libc.h" resolve to musl's own headers.
# arch/x86_64 provides syscall_arch.h (inline __syscallN stubs); arch/generic is the
# bits fallback. user/include carries the generated bits/syscall.h + xos headers.
# Relaxed warnings (-Wno-all): upstream musl is third-party code, not under our
# -Werror gate (same rationale as add_drm_lib for third_party/drm).
target_include_directories(musl_unistd_objs PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi
)
target_compile_options(musl_unistd_objs PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -fno-pie -Wno-all)

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
    lib/musl_shim/syscall_cp.c
)
target_include_directories(musl_unistd_objs_so PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi
)
target_compile_options(musl_unistd_objs_so PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -fPIC -Wno-all)
# The SHARED libc.so link consumes the PIC objects as bare .o files; the STATIC
# libc.a path consumes the -fno-pie objects via $<TARGET_OBJECTS:...>. Both are
# wired through EXTRA_OBJS below.

# ===================== musl fcntl integration (fcntl_worklist §3d) =====================
# Build the upstream musl src/fcntl/*.c into libc via a separate OBJECT library,
# mirroring musl_unistd_objs (same musl-internal include order, same shim).
# musl's open.c / openat.c / fcntl.c / creat.c / posix_fadvise.c /
# posix_fallocate.c replace the repo's hand-written file.cc wrappers —
# open/openat/fcntl are deleted from file.cc this batch.
#
# openat.c IS adopted: musl's openat passes AT_FDCWD straight to SYS_openat,
# and the kernel's sys_openat resolves AT_FDCWD to the process cwd (the M0.4
# resolve_dirfd_start fix in kernel/bsd/vfs.c resolves bp->cwd to its inode),
# so openat(AT_FDCWD, rel) honors a prior chdir. The repo's file.cc openat
# wrapper (which prepended a userspace cwd copy via resolve_at_path) and the
# fcntl_ext.h LIBC_EXPORT re-declaration are deleted.
#
# open.c routes to SYS_open (SYS_open is defined in bits/syscall.h, so musl's
# __sys_open_cp picks the 2/3-arg SYS_open path, not SYS_openat). The kernel's
# sys_open resolves relatives against bp->cwd via vfs_resolve_user. musl chdir
# (adopted via musl_unistd_objs) keeps bp->cwd in sync.
#
# No need to pull syscall_ret.c / __syscall_cp.c here: __syscall_ret and
# __syscall_cp are provided by musl_pthread (the single source for both);
# the fcntl wrappers' syscall(...) / syscall_cp(...) / __syscall(...) resolve
# at link time.
set(MUSL_FCNTL_SOURCES
    ${MUSL_DIR}/src/fcntl/open.c
    ${MUSL_DIR}/src/fcntl/openat.c
    ${MUSL_DIR}/src/fcntl/fcntl.c
    ${MUSL_DIR}/src/fcntl/creat.c
    ${MUSL_DIR}/src/fcntl/posix_fadvise.c
    ${MUSL_DIR}/src/fcntl/posix_fallocate.c)

add_library(musl_fcntl_objs OBJECT ${MUSL_FCNTL_SOURCES})
target_include_directories(musl_fcntl_objs PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_fcntl_objs PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -fno-pie -Wno-all)

# libc.so needs PIC objects (mirror the libc.a(-fno-pie)/libc.so(-fPIC) dual
# build): a second OBJECT library, musl_fcntl_objs_so, compiles the SAME musl
# sources with -fPIC for the shared link.
add_library(musl_fcntl_objs_so OBJECT ${MUSL_FCNTL_SOURCES})
target_include_directories(musl_fcntl_objs_so PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_fcntl_objs_so PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -fPIC -Wno-all)

# ===================== musl dlfcn integration (dlfcn) =====================
# Build the upstream musl src/ldso/{dlsym,dlclose,dlinfo,dlerror}.c into libc,
# mirroring musl_fcntl_objs (same musl-internal include order, same dual
# -fno-pie/-fPIC build). dlsym/dlclose/dlinfo wrap symbols the fused loader
# already defines in dynlink.c (__dlsym, __dl_invalid_handle), so they pull the
# non-stub implementations in over musl's weak stubs (src/ldso/*.c ship a
# weak_alias "not supported" fallback that dynlink.c's strong defs override).
# dlerror.c provides the real dlerror()/__dl_seterr/__dl_vseterr: now that the
# runtime fs:0 points at musl's struct pthread (musl_pthread provides the full
# layout incl. dlerror_buf/dlerror_flag), musl's dlerror.c reads/writes the
# correct offsets — no stub needed.
#
# NOT included here:
#   dlopen.c / dladdr.c / dl_iterate_phdr.c — strong definitions already live
#     in the fused ldso/dynlink.c (spliced into libc.so via musl_loader_objs);
#     the weak stubs in src/ldso/*.c would only be needed for a static build
#     without the loader, which we don't have.
#   __dlsym.c — provides a weak stub __dlsym; dynlink.c already defines the
#     strong one, so the stub is dead weight. dlsym.c (the public wrapper) is
#     all we need.
# dlsym.c/dlclose.c/dlinfo.c need _GNU_SOURCE for dlfcn.h to declare dlinfo
# (glibc extension) and for link.h's struct link_map (dlinfo writes one).
# NOTE: dlinfo.c #defines _GNU_SOURCE itself, so we do NOT pass -D_GNU_SOURCE
# on the command line (would trip -Wmacro-redefined). dlsym.c/dlclose.c don't
# need it — dlsym/dlclose are declared unconditionally in dlfcn.h.
set(MUSL_DL_SOURCES
    ${MUSL_DIR}/src/ldso/dlsym.c
    ${MUSL_DIR}/src/ldso/dlclose.c
    ${MUSL_DIR}/src/ldso/dlinfo.c
    ${MUSL_DIR}/src/ldso/dlerror.c)

add_library(musl_dl_objs OBJECT ${MUSL_DL_SOURCES})
target_include_directories(musl_dl_objs PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_dl_objs PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -fno-pie -Wno-all)

# libc.so PIC mirror (same as musl_fcntl_objs_so above).
add_library(musl_dl_objs_so OBJECT ${MUSL_DL_SOURCES})
target_include_directories(musl_dl_objs_so PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_dl_objs_so PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -fPIC -Wno-all)

# ===================== musl dirent integration (dirent.md) =====================
# Build the upstream musl src/dirent/*.c into libc, mirroring musl_fcntl_objs
# (same musl-internal include order, same dual -fno-pie/-fPIC build). These
# replace the repo's hand-written opendir/readdir/closedir/seekdir/telldir/
# rewinddir/dirfd/readdir_r/scandir block that lived in user/lib/file.cc
# (deleted this batch).
#
# Layout match is the enabler: musl's struct dirent { ino_t d_ino; off_t d_off;
# unsigned short d_reclen; unsigned char d_type; char d_name[256]; } is
# field-for-field identical to the kernel's struct dirent64
# (include/uapi/xos/dirent.h), so musl's readdir() returns a pointer straight
# into the DIR's getdents buffer — no per-field copy. fat32 already fills
# d_type (DT_DIR/DT_REG), which the old hand-written readdir discarded.
#
# musl's readdir.c calls __syscall(SYS_getdents, …) = 78 directly; the kernel
# aliases SYS_GETDENTS (78) onto sys_getdents in kernel/bsd/syscall.c (returns
# dirent64 records), so musl's path resolves with NO libc-side glue. __getdents.c
# provides the public getdents() weak alias (kept for the Linux ABI surface; it
# likewise routes through 78).
#
# LOCK/UNLOCK (used by readdir_r/seekdir/rewinddir) resolve to __lock/__unlock
# from musl_pthread's src/thread/__lock.c (file(GLOB …/src/thread/*.c)). No extra
# source needed. fdopendir.c's fstat + fcntl deps come from file.cc's statx
# AT_EMPTY_PATH fstat and musl_fcntl_objs respectively.
#
# alphasort.c is intentionally EXCLUDED (dirent.md §4a): it calls strcoll, which
# drags in the locale chain — not worth it for a newly-added API no in-tree
# consumer uses. versionsort.c IS included; its only dep strverscmp.c
# (src/string/, no locale) is pulled in below. user/include/dirent.h does not
# declare alphasort.
set(MUSL_DIRENT_SOURCES
    ${MUSL_DIR}/src/dirent/opendir.c
    ${MUSL_DIR}/src/dirent/readdir.c
    ${MUSL_DIR}/src/dirent/readdir_r.c
    ${MUSL_DIR}/src/dirent/closedir.c
    ${MUSL_DIR}/src/dirent/seekdir.c
    ${MUSL_DIR}/src/dirent/telldir.c
    ${MUSL_DIR}/src/dirent/rewinddir.c
    ${MUSL_DIR}/src/dirent/dirfd.c
    ${MUSL_DIR}/src/dirent/scandir.c
    ${MUSL_DIR}/src/dirent/__getdents.c
    ${MUSL_DIR}/src/dirent/fdopendir.c
    ${MUSL_DIR}/src/dirent/versionsort.c
    ${MUSL_DIR}/src/string/strverscmp.c)   # versionsort's only dependency

add_library(musl_dirent_objs OBJECT ${MUSL_DIRENT_SOURCES})
target_include_directories(musl_dirent_objs PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_dirent_objs PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -fno-pie -Wno-all)

# libc.so PIC mirror (same as musl_fcntl_objs_so above).
add_library(musl_dirent_objs_so OBJECT ${MUSL_DIRENT_SOURCES})
target_include_directories(musl_dirent_objs_so PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_dirent_objs_so PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -fPIC -Wno-all)

# ===================== musl pthread integration (pthread.md §八) =====================
# Generate musl's build-time headers (bits/alltypes.h + bits/syscall.h) into
# ${CMAKE_BINARY_DIR}/musl_gen BEFORE any musl source or consumer compiles.
# add_musl_lib depends on the musl_headers target; the global include path
# added below points consumers at musl_gen so <bits/alltypes.h> resolves.
musl_generate_headers()

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
    # __stdin_used/__stdout_used/__stderr_used. We keep our own stdio
    # (user/lib/stdio.cc) — our FILE objects don't register on musl's ofl list,
    # and the three *_used symbols are weak-aliased to a NULL dummy
    # (src/stdio/__stdio_exit.c:3-6), so init_file_lock(NULL) is a no-op
    # (pthread_create.c:179-182) and the ofl walk iterates an empty list. These
    # two files are compiled only to PROVIDE those symbols so the dynamic
    # linker stops failing libc.so relocation with "unresolved PLT symbol
    # __ofl_unlock"; at runtime they do nothing.
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/stdio/ofl.c
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/stdio/__stdio_exit.c
    # __lockfile: pulled in by __stdio_exit.c's close_file() via FFINALLOCK.
    # Its deps (__pthread_self, a_cas, __wait, a_store, __wake) are all already
    # in musl_pthread, so it adds no further undefined chain. It operates on
    # musl FILE->lock/waiters, which is fine — our stdio never enters musl's
    # ofl list, so __lockfile is only reachable via __stdio_exit (which we do
    # not call; our exit path is __libc_run_atexit + fflush(stdout)).
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
    # musl-side startup bridge: sets libc.page_size/auxv for our __libc_start_main
    # (compiled here so it sees musl's private struct __libc via libc.h).
    ${CMAKE_SOURCE_DIR}/user/lib/musl_startup.c
)
add_musl_lib(musl_pthread SOURCES ${MUSL_PTHREAD_SOURCES})

# libc.a (static, -fno-pie -DDYNAMIC=0)
# IMAGE_PATH usr/lib/libc.a: ship the static libc.a into the root FS (STATIC has
# no auto-default). IMAGE_ARTIFACT libc.a overrides the default lib<c>.a build name.
# EXTRA_OBJS: musl unistd + fcntl upstream objects (unistd_worklist M0.2/M0.3,
# fcntl_worklist §3d) merged into the archive, replacing the repo's hand-written
# wrappers, AND musl_pthread so every static-linked ELF automatically gets
# __init_tls, __errno_location, pthread_*, sigemptyset/..., __restore_rt, struct
# __libc — required because errno lives at %fs:0 (musl TCB) and EVERY ELF (even
# single-threaded hello) must run __init_tls at startup or the first errno fetch
# crashes. musl_pthread is built -fPIC by add_musl_lib (one compile serves both
# the static -no-pie ELF and the shared libc.so links).
add_user_lib(c
    SOURCES ${LIBC_SOURCES}
    EXTRA_OBJS $<TARGET_OBJECTS:musl_unistd_objs> $<TARGET_OBJECTS:musl_fcntl_objs>
               $<TARGET_OBJECTS:musl_dl_objs> $<TARGET_OBJECTS:musl_dirent_objs>
               $<TARGET_OBJECTS:musl_pthread>
    C
    FLAGS "-fno-pie -DDYNAMIC=0"
    IMAGE_PATH usr/lib/libc.a
    IMAGE_ARTIFACT libc.a
)

# libc.so (dynamic, -fPIC -DDYNAMIC=1) — FUSED musl loader + hand-written libc
# (ldso.md). The musl loader objects (dynlink.lo/dlstart.lo, PIC) are spliced in
# via EXTRA_OBJS; -e _dlstart makes the loader entry the .so entry point. The
# loader's musl-internal symbol dependencies (__libc/__init_tp/__copy_tls/
# __block_all_sigs/... ) are satisfied by the musl_pthread objects below — the
# loader runs musl's real TLS setup during __dls3 (FS_BASE → musl struct
# pthread), and __libc_start_main does NOT override fs_base. Only the
# loader-only, non-pthread symbols (dprintf/getdelim/__tlsdesc_*/__libc_get_version)
# still come from lib/musl_loader_shim.c (in LIBC_SOURCES). After bootstrap the
# loader jumps to the main ELF _start; the hand-written __libc_start_main then
# runs the loader's __libc_start_init() (do_init_fni) over every DSO's
# .init_array (loader dormant thereafter — ldso.md §2).
# Merges musl_pthread's objects too so libc.so carries __init_tls/__errno_location/
# pthread_*/sigset helpers — the dynamic loader and every dyn ELF depend on
# libc.so for these.
add_user_lib(c_so
    SOURCES ${LIBC_SOURCES}
    EXTRA_OBJS $<TARGET_OBJECTS:musl_unistd_objs_so> $<TARGET_OBJECTS:musl_fcntl_objs_so>
               $<TARGET_OBJECTS:musl_dl_objs_so> $<TARGET_OBJECTS:musl_dirent_objs_so>
               $<TARGET_OBJECTS:musl_loader_objs> $<TARGET_OBJECTS:musl_pthread>
    C
    FLAGS "-fPIC -DDYNAMIC=1"
    SHARED
    OUTPUT_NAME c
    ENTRY _dlstart
    VERSION_MAP user/libc.map
    # NOTE: no SO_LINK_LIBS — libc.so is the base library, does not record DT_NEEDED on itself
)
add_dependencies(c_so musl_libc)
# Publish the fused libc.so under BOTH names (ldso.md §3 Phase 2, mirroring
# musl Makefile:214's hardlink): the SHARED auto-default already ships it to
# /lib/libc.so (DT_NEEDED soname target); this extra entry ships the SAME
# artifact to /lib/ld-musl-x86_64.so.1 (PT_INTERP path the kernel opens).
os_image_path(c_so libc.so lib/ld-musl-x86_64.so.1 PARTITION 2)

# libm source list
set(LIBM_SOURCES
    lib/math/math_basic.c
    lib/math/math_float.c
    lib/math/sincos.c
)

# libm.a (static)
add_user_lib(m
    SOURCES ${LIBM_SOURCES}
    C
    FLAGS "-fno-pie -D__LIBM_BUILD__"
)

# libm.so (dynamic)
add_user_lib(m_so
    SOURCES ${LIBM_SOURCES}
    C
    FLAGS "-fPIC -DDYNAMIC=1 -D__LIBM_BUILD__ -fvisibility=default"
    SHARED
    OUTPUT_NAME m
    VERSION_MAP user/libm.map
)


# Unity test framework (third_party 例外：编译期符号依赖，STATIC 链入 test ELF)
# add_third_party_lib(unity STATIC ...)：关 warning(-w)，仅 link os_base_options 取共性基础项，
# 不被我方 -Werror 门禁污染。test ELF 链 unity 替代内联 unity.c（消除 28 处内联 + 重复 DEFS）。
# UNITY_EXCLUDE_MATH_H：排除 host <math.h>。freestanding + -nostdlib 下 <sys/cdefs.h>
# 不被拉入，host math.h 的 __BEGIN_DECLS 等宏无法解析；Unity 自带 UNITY_IS_NAN/IS_INF fallback。
# 注意不要再加 UNITY_EXCLUDE_SETJMP_H：那会把 TEST_ABORT() 退化为 return，断言失败后
# 测试继续执行（NULL 级联解引用杀掉整个测试二进制，bug.md Bug 1）。user/include/setjmp.h
# + lib/setjmp.S 提供可用的 setjmp/longjmp，断言失败经 longjmp 正确中止单个测试。
# 定义挂在 unity target，经 INTERFACE 传给链入它的 test ELF。
add_third_party_lib(unity STATIC
    SOURCES ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/user/include
        ${CMAKE_SOURCE_DIR}/third_party/Unity/src
    INTERFACE_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/Unity/src)
target_compile_definitions(unity PUBLIC UNITY_EXCLUDE_MATH_H UNITY_INCLUDE_DOUBLE)

add_user_elf(hello  C SOURCES hello.c  LINK_LIBS c IMAGE_PATH local/hello.elf)
# test_fpu / ld_test_* / drm_test_link are diagnostic ELFs shipped only under /test/.
# In non-TEST builds they still compile (consumed nowhere at runtime — test_runner
# itself is TEST-gated in user/test/), but are NOT shipped to the image, preserving
# the pre-manifest mkdisk behavior (those ELFs were never in the non-TEST image).
# The TEST flag selects whether to register the manifest entry.
if(TEST)
    add_user_dyn_elf(test_fpu C SOURCES test/test_fpu.c LINK_LIBS c)
else()
    add_user_dyn_elf(test_fpu C SOURCES test/test_fpu.c LINK_LIBS c NO_IMAGE)
endif()

# hello_dyn (phase 2b+3: full dynamic chain verification)
# LINK_LIBS c -> -lc resolves to libc.so (dynamic link, records DT_NEEDED libc.so)
# add_user_dyn_elf 内部已自动解析 LINK_LIBS→<lib>_so/lib<lib>_so target 并 add_dependencies
# （见 user_rules.cmake），无需手写 add_dependencies(hello_dyn_dyn_elf c_so)。
# IMAGE_PATH local/hello_dyn.elf: ship to /local/ (override dyn-elf default test/).
add_user_dyn_elf(hello_dyn C SOURCES hello_dyn.c LINK_LIBS c IMAGE_PATH local/hello_dyn.elf)

# terminal — dynamic ELF (libc.so + libinput.so), PT_INTERP → ld.so
# IMAGE_PATH usr/bin/terminal: override dyn-elf default test/.
add_user_dyn_elf(terminal
    SOURCES driver/terminal_libinput.cc
    LINK_LIBS input drm m c udev
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/libinput/src;${CMAKE_SOURCE_DIR}/third_party/libinput/include;${CMAKE_SOURCE_DIR}/user/lib/udev-shim;${CMAKE_SOURCE_DIR}/user/lib/evdev-shim;${CMAKE_SOURCE_DIR}/third_party/drm;${CMAKE_SOURCE_DIR}/third_party/drm/include/drm"
    IMAGE_PATH usr/bin/terminal
)

# evdev: real keyboard event source (xHCI HID → SHM ring → terminal) +
# EVIOCG* ioctl query. Replaces the kbd driver for terminal input.
# IMAGE_PATH driver/evdev.dev: rename evdev.elf → driver/evdev.dev in the image.
add_user_elf(evdev  SOURCES driver/evdev.cc lib/usb_kbd.cc  LINK_LIBS c IMAGE_PATH driver/evdev.dev)

# ===== libinput (static library, keyboard subset) =====
# Provides libinput event processing pipeline. Wraps libevdev/udev shims.
set(LIBINPUT_SOURCES
    # libinput core (keyboard-only subset)
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev-fallback.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev-middle-button.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev-plugin.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput-plugin.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput-plugin-button-debounce.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput-plugin-mouse-wheel.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/path-seat.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/udev-seat.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/timer.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-libinput.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/quirks.c
    # filters
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/filter.c
    # utilities
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-files.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-list.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-ratelimit.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-strings.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-prop-parsers.c
    # os-test compat layer (scandir etc.)
    lib/libinput-compat/compat.c
    lib/libinput-compat/stubs.c
    lib/fnmatch.c
    # evdev/udev shims (needed by libinput internal)
    lib/evdev-shim/evdev.c
    # udev.c 不在此:libinput.so 经 SO_LINK_LIBS udev 链 libudev.so 取 udev_*
    # (完整 Linux 模式:libudev 独立 .so,见下方 libudev_so)。
)

# Shared warning-suppression list for the libinput third-party sources, used by
# both the static `input` library (below) and the shared `libinput.so` (further
# below). libinput upstream relies on implicit function declarations and other
# patterns that clang treats as hard errors in C11+ (not downgraded by -w), so
# these must be suppressed explicitly per-target. -Wno-unknown-warning-option
# lets the same list pass under gcc (which doesn't know the clang-only entries).
set(LIBINPUT_WARN_FLAGS
    "-Wno-unused-parameter -Wno-duplicate-decl-specifier -Wno-implicit-function-declaration -Wno-return-type -Wno-int-conversion -Wno-format -Wno-implicit-fallthrough -Wno-sign-compare -Wno-error=cpp -Wno-maybe-uninitialized -Wno-sometimes-uninitialized -Wno-unknown-warning-option")

add_user_lib(input
    SOURCES ${LIBINPUT_SOURCES}
    C
    FLAGS "-O2 -DLIBINPUT_QUIRKS_DIR=\\\"/usr/share/libinput\\\" -DHAVE_QUIRKS ${LIBINPUT_WARN_FLAGS}"
    OUTPUT_NAME input
)

set(LIBINPUT_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include/uapi
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi/compat
    ${CMAKE_SOURCE_DIR}/third_party/libinput/include
    ${CMAKE_SOURCE_DIR}/third_party/libinput
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src
    ${CMAKE_SOURCE_DIR}/user/lib/libinput-config
    ${CMAKE_SOURCE_DIR}/user/lib/evdev-shim
    ${CMAKE_SOURCE_DIR}/user/lib/udev-shim
    ${CMAKE_SOURCE_DIR}/user/lib/libinput-compat
)

target_include_directories(input PRIVATE ${LIBINPUT_INCLUDE_DIRS})

# libinput.so (shared library, third_party SHARED-only, dynamically linked into terminal.elf)
# add_third_party_lib（分支 B）关 warning(-w)、用 os_base_options 等价基础项。-fvisibility=hidden
# + libinput 的 LIBINPUT_EXPORT(visibility("default")) 标记公共 API，仅导出公共符号。
# SO_LINK_LIBS c udev 记 DT_NEEDED libc.so + libudev.so。
add_third_party_lib(libinput_so
    SOURCES ${LIBINPUT_SOURCES}
    C
    OUTPUT_NAME input
    SO_LINK_LIBS c udev
    FLAGS "-O2 -DHAVE_QUIRKS ${LIBINPUT_WARN_FLAGS}"
    INCLUDE_DIRS ${LIBINPUT_INCLUDE_DIRS}
)

# libudev.so (独立共享库,完整 Linux 模式,third_party SHARED-only):udev-shim 的
# udev_* API 导出供终端 main(libinput_udev_create_context 前的 udev_new())与 libinput.so
# 内部(udev-seat.c 调 udev_*)共享。add_third_party_lib（分支 B）关 warning(-w)、用
# os_base_options 等价基础项。FLAGS 追加 -fvisibility=default 覆盖默认 hidden——udev.c 内
# 非 static 函数全为公共 udev_* API(无内部泄漏),故默认可见即全导出,无需逐个 UDEV_EXPORT 标记。
# 对齐 Linux:libudev 与 libinput 分离;静态消费者(test_udevd/test_libudev/test_udevd_db
# 各自 SOURCES 编 udev.c)不受影响。
add_third_party_lib(libudev_so
    SOURCES lib/udev-shim/udev.c
    C
    OUTPUT_NAME udev
    SO_LINK_LIBS c
    FLAGS "-fvisibility=default -O2"
)

if(TEST)
    add_user_elf(shell SOURCES shell/shell.cc LINK_LIBS c DEFS TEST IMAGE_PATH usr/bin/shell)
else()
    add_user_elf(shell SOURCES shell/shell.cc LINK_LIBS c IMAGE_PATH usr/bin/shell)
endif()

# udev/ — device event daemon (subscribes to netlink uevent group)
add_user_elf(udevd C SOURCES udev/udevd.c LINK_LIBS c IMAGE_PATH usr/bin/udevd)

# ===== ld.so multi-dependency unit tests (ld.md §8 / plan_ld phase D) =====
# Fully decoupled from drm: use stub .so topologies + a dynamic main ELF to
# cover the loading layer (recursive load / dedup / cycle detection).
# stub .so does not go through add_user_lib(SHARED) (that path is tied to
# libc.map + libc export validation); here we use standalone custom commands,
# matching the ld.so/libc.so pattern (gcc -shared -fPIC -Wl,-soname).
#
# Cycle scenario needs a<->b mutual NEEDED (plan_ld D7). Mutual references have
# a link-time bootstrap problem (when linking a, b is not yet built). Solution:
#   1. libb.so.seed: empty .so (SONAME=libb.so, only NEEDED libc.so) as
#      bootstrap seed.
#   2. liba.so: link ld_stub_a.c + directly reference libb.so.seed
#      (--no-as-needed), records NEEDED libb.so (takes the seed's SONAME) +
#      libc.so. liba does not call any b symbol, no symbol resolution needed,
#      DT_NEEDED is purely a record.
#   3. libb.so: link ld_stub_b.c + directly reference liba.so (--no-as-needed)
#      + libc.so. libb calls lda_answer (from liba) — --allow-shlib-undefined
#      tolerates this undefined reference, records NEEDED liba.so + libc.so.
# Chain/diamond scenarios reuse the same pair of stubs; dedup guarantees
# correctness.

# 1. libb.so.seed: bootstrap seed (empty .so, SONAME=libb.so, only NEEDED libc.so)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/libb.so.seed
    COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,libb.so
            -o ${CMAKE_BINARY_DIR}/libb.so.seed -x c /dev/null
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking libb.so.seed (ld.so stub bootstrap)")
# 2. liba.so: NEEDED libb.so (seed SONAME) + libc.so
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/liba.so
    COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,liba.so
            -Wl,--no-as-needed -Wl,--allow-shlib-undefined
            -o ${CMAKE_BINARY_DIR}/liba.so ${CMAKE_SOURCE_DIR}/user/test/ld_stub_a.c
            ${CMAKE_BINARY_DIR}/libb.so.seed
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_SOURCE_DIR}/user/test/ld_stub_a.c ${CMAKE_BINARY_DIR}/libb.so.seed ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking liba.so (ld.so stub, NEEDED libb.so+libc.so)")
add_custom_target(ld_stub_a_so ALL DEPENDS ${CMAKE_BINARY_DIR}/liba.so)
add_dependencies(ld_stub_a_so c_so)
# Ship liba.so to /test/lib/liba.so (ld.so load fallback path — separate from
# production /lib/; standalone custom targets aren't covered by helper defaults).
os_image_path(ld_stub_a_so liba.so test/lib/liba.so)
# 3. libb.so: NEEDED liba.so + libc.so (lda_answer reference tolerated via --allow-shlib-undefined)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/libb.so
    COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,libb.so
            -Wl,--no-as-needed -Wl,--allow-shlib-undefined
            -o ${CMAKE_BINARY_DIR}/libb.so ${CMAKE_SOURCE_DIR}/user/test/ld_stub_b.c
            ${CMAKE_BINARY_DIR}/liba.so
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_SOURCE_DIR}/user/test/ld_stub_b.c ${CMAKE_BINARY_DIR}/liba.so ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking libb.so (ld.so stub, NEEDED liba.so+libc.so)")
add_custom_target(ld_stub_b_so ALL DEPENDS ${CMAKE_BINARY_DIR}/libb.so)
add_dependencies(ld_stub_b_so ld_stub_a_so c_so)
os_image_path(ld_stub_b_so libb.so test/lib/libb.so)

# 4 dynamic main ELFs: one per scenario, hooked into test_runner.c's tests[] table (D9)
# Diagnostic-only (test_runner is TEST-gated): ship to /test/ only in TEST builds.
if(TEST)
    set(_LD_TEST_NO_IMAGE "")
else()
    set(_LD_TEST_NO_IMAGE NO_IMAGE)
endif()
# Scenario 1 single-dependency regression
add_user_dyn_elf(ld_test_single C SOURCES test/ld_test_single.c LINK_LIBS c ${_LD_TEST_NO_IMAGE})
# Scenario 2 linear chain (main -> b -> {a, libc})
add_user_dyn_elf(ld_test_chain C SOURCES test/ld_test_chain.c LINK_LIBS b a c ${_LD_TEST_NO_IMAGE})
add_dependencies(ld_test_chain_dyn_elf ld_stub_b_so ld_stub_a_so c_so)
# Scenario 3 diamond dedup (main -> {a, libc}, a -> libc)
add_user_dyn_elf(ld_test_diamond C SOURCES test/ld_test_diamond.c LINK_LIBS a c ${_LD_TEST_NO_IMAGE})
add_dependencies(ld_test_diamond_dyn_elf ld_stub_a_so c_so)
# Scenario 4 cycle detection (a<->b mutual NEEDED)
add_user_dyn_elf(ld_test_cycle C SOURCES test/ld_test_cycle.c LINK_LIBS a b c ${_LD_TEST_NO_IMAGE})
add_dependencies(ld_test_cycle_dyn_elf ld_stub_a_so ld_stub_b_so c_so)

# ===== libdrm (third_party/drm, core only) — plan_drm2 =====
# libdrm 公共编译参数 (libdrm.md §3.3)
#
# config.h / libdrm.map live in the MAIN repo (build_script/libdrm/), not in
# the third_party/drm submodule — we must not pollute the upstream submodule
# working tree. CMake copies them into the build tree (configure_file) so the
# -include path resolves to build/libdrm_config.h and the (phase-4) version
# script to build/libdrm.map. generated_static_table_fourcc.h is produced by
# upstream's gen_table_fourcc.py (add_custom_command) so the build is fully
# reproducible with no manual one-shot steps.
configure_file(${CMAKE_SOURCE_DIR}/build_script/libdrm/config.h
               ${CMAKE_BINARY_DIR}/libdrm_config.h COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/build_script/libdrm/libdrm.map
               ${CMAKE_BINARY_DIR}/libdrm.map COPYONLY)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated_static_table_fourcc.h
    COMMAND python3
            ${CMAKE_SOURCE_DIR}/third_party/drm/gen_table_fourcc.py
            ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm/drm_fourcc.h
            ${CMAKE_BINARY_DIR}/generated_static_table_fourcc.h
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm/drm_fourcc.h
            ${CMAKE_SOURCE_DIR}/third_party/drm/gen_table_fourcc.py
    COMMENT "Generating libdrm static fourcc table")
add_custom_target(libdrm_fourcc_table
    DEPENDS ${CMAKE_BINARY_DIR}/generated_static_table_fourcc.h)

set(LIBDRM_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drm.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmMode.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmHash.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmRandom.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmSL.c)

# libdrm.a (static, -fno-pie, relaxed warnings — upstream submodule code)
# INCLUDE_DIRS: private paths for compiling libdrm sources (upstream <drm.h>,
# xf86drm internal headers, build dir for config.h/fourcc table).
# INTERFACE_INCLUDE_DIRS: propagated to any target that links drm — provides
# xf86drm.h / xf86drmMode.h location + upstream <drm.h> resolution path,
# so consumers don't need explicit INCLUDE_DIRS.
add_drm_lib(drm C SOURCES ${LIBDRM_SOURCES}
            INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm
                         ${CMAKE_SOURCE_DIR}/third_party/drm
                         ${CMAKE_BINARY_DIR}
            INTERFACE_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm
                                   ${CMAKE_SOURCE_DIR}/third_party/drm
            FLAGS "-Wno-unused-parameter -Wno-format -D__linux__ -include ${CMAKE_BINARY_DIR}/libdrm_config.h")
# ensure generated_static_table_fourcc.h exists before libdrm sources compile
add_dependencies(drm libdrm_fourcc_table)

# libdrm.a 验证小程序 (libdrm.md §3.4: drmOpen + drmModeGetResources)
# xf86drm.h / xf86drmMode.h and upstream <drm.h> resolution paths
# propagate automatically from the drm target's INTERFACE_INCLUDE_DIRS.
# IMAGE_PATH test/drm_test_link.elf: ship to /test/ in TEST builds only (diagnostic;
# test_runner is TEST-gated). In non-TEST builds, NO_IMAGE keeps it out of the image
# (matching the pre-manifest mkdisk, which never shipped it outside the TEST loop).
if(TEST)
    add_user_elf(drm_test_link C SOURCES test/drm_test_link.c LINK_LIBS drm c IMAGE_PATH test/drm_test_link.elf)
else()
    add_user_elf(drm_test_link C SOURCES test/drm_test_link.c LINK_LIBS drm c NO_IMAGE)
endif()

# ===== libdrm.so (shared library, third_party SHARED-only) — Phase A =====
# add_third_party_lib（分支 B：custom-command .so）关 warning(-w)、用 os_base_options
# 等价基础项，不携带我方 WARN_FLAGS(-Werror)，实现第三方 warning 自治。SO_LINK_LIBS c
# 记 DT_NEEDED libc.so。LINK_DEPS 挂 fourcc 表生成 target。-fvisibility=hidden + drm_public
# (via HAVE_VISIBILITY) gate exports（无 VERSION_MAP：verify_libc_exports.sh 不处理 drm* glob）。
add_third_party_lib(drm_so
    SOURCES ${LIBDRM_SOURCES}
    C
    OUTPUT_NAME drm
    SO_LINK_LIBS c
    LINK_DEPS libdrm_fourcc_table
    FLAGS "-Wno-unused-parameter -Wno-format -D__linux__ -include ${CMAKE_BINARY_DIR}/libdrm_config.h"
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm
                 ${CMAKE_SOURCE_DIR}/third_party/drm
                 ${CMAKE_BINARY_DIR}
)

# ===== libffi (third_party/libffi, full-feature incl. closures) — ffi_worklist =====
# fficonfig.h: hand-written fixed template (COPYONLY copy into build tree).
# Pre-defines FFI_MMAP_EXEC_SELINUX/PAX=0 to skip /proc/mounts/statfs probing,
# HAVE_MEMFD_CREATE=1 so closures.c open_temp_exec_file picks memfd first, and
# HAVE_MREMAP=0 (kernel has no mremap). See libffi.md §3.2 / ffi_worklist §1.
configure_file(${CMAKE_SOURCE_DIR}/build_script/libffi/fficonfig.h
               ${CMAKE_BINARY_DIR}/fficonfig.h COPYONLY)

# ffitarget.h: upstream public header (include/Makefile.am:9 nodist_include_HEADERS
# = ffi.h ffitarget.h). ffi.h:86 does `#include <ffitarget.h>`, so every consumer
# of <ffi.h> transitively needs it. The per-arch copies live under src/<arch>/
# (noinst_HEADERS, Makefile.am:53); configure picks the current arch's copy into
# the public include dir. Mirror that: COPYONLY the x86-64 ffitarget.h into the
# build tree next to ffi.h/fficonfig.h, so consumers (test_ffi) and ffi_so itself
# both resolve <ffitarget.h> from the single public dir ${CMAKE_BINARY_DIR} —
# nobody reaches into src/x86/.
configure_file(${CMAKE_SOURCE_DIR}/third_party/libffi/src/x86/ffitarget.h
               ${CMAKE_BINARY_DIR}/ffitarget.h COPYONLY)

# ffi.h: generated from ffi.h.in by @-substitution. The 6 placeholders in
# ffi.h.in are @TARGET@/@HAVE_LONG_DOUBLE@/@FFI_EXEC_TRAMPOLINE_TABLE@/
# @VERSION@/@FFI_VERSION_STRING@/@FFI_VERSION_NUMBER@ (NO LIBFFI_ prefix —
# verified). configure_file substitutes @VAR@ by exact CMake variable name, so
# the variables MUST share the placeholder names. Wrap in a function() so the
# short names TARGET/VERSION do not leak into the parent scope.
function(_libffi_generate_ffi_header)
    set(TARGET X86_64)                       # ffitarget.h:57
    set(VERSION 3.7.1)                       # configure.ac:5 AC_INIT
    set(HAVE_LONG_DOUBLE 1)                  # x86-64 long double = 80-bit x87
    set(FFI_VERSION_STRING "3.7.1")
    set(FFI_VERSION_NUMBER 30701)            # 3*10000 + 7*100 + 1
    set(FFI_EXEC_TRAMPOLINE_TABLE 0)         # no tramp.c → dynamic trampoline
    configure_file(${CMAKE_SOURCE_DIR}/third_party/libffi/include/ffi.h.in
                   ${CMAKE_BINARY_DIR}/ffi.h)
endfunction()
_libffi_generate_ffi_header()

set(LIBFFI_DIR ${CMAKE_SOURCE_DIR}/third_party/libffi)
set(LIBFFI_SOURCES
    ${LIBFFI_DIR}/src/prep_cif.c
    ${LIBFFI_DIR}/src/types.c
    ${LIBFFI_DIR}/src/raw_api.c
    ${LIBFFI_DIR}/src/java_raw_api.c
    ${LIBFFI_DIR}/src/debug.c
    ${LIBFFI_DIR}/src/x86/ffi64.c
    ${LIBFFI_DIR}/src/x86/unix64.S
    ${LIBFFI_DIR}/src/x86/ffiw64.c
    ${LIBFFI_DIR}/src/x86/win64.S
    ${LIBFFI_DIR}/src/closures.c
    ${LIBFFI_DIR}/src/tramp.c
)
set(LIBFFI_INCLUDE_DIRS
    ${CMAKE_BINARY_DIR}              # <ffi.h> <fficonfig.h> <ffitarget.h>(生成/拷贝)
    ${LIBFFI_DIR}/include            # <tramp.h> <ffi_common.h>
    ${LIBFFI_DIR}/src                # dlmalloc.c(closures.c #include)
    ${LIBFFI_DIR}                    # 兜底
)

# libffi.so (shared, full-feature incl. ffi_closure). ffi.h.in's FFI_API is
# an empty define on non-Windows (ffi.h.in:152), so libffi does NOT mark
# exports with visibility("default") — mirror libudev.so: FLAGS adds
# -fvisibility=default to override add_user_lib(SHARED)'s default hidden
# (user_rules.cmake:109), exporting all non-static symbols. -Wno-error
# suppresses WARN_FLAGS' -Werror (dlmalloc is 6443 lines; per-warning
# -Wno-* is impractical) while keeping -Wall -Wextra visible.
# dlmalloc.c is NOT in SOURCES — closures.c:570 #includes it textually.
# tramp.c IS compiled but FFI_EXEC_STATIC_TRAMP is undefined, so only its
# #else stub branch builds (ffi_tramp_is_supported()→0, ffi_tramp_alloc/free
# no-ops). closures.c:901/1003/1029/1050 gate calls on ffi_tramp_is_supported(),
# so the stubs are never hit at runtime — but the link-time symbol refs must
# resolve, else libffi.so's PLT leaves ffi_tramp_free unresolved and the
# dynamic loader FATALs on reloc (was the test_ffi failure).
# ffiw64.c + win64.S provide the FFI_EFI64/FFI_GNUW64 (Windows x64) ABI path.
# ffitarget.h:134 unconditionally #defines FFI_GO_CLOSURES, so ffi64.c's
# ffi_call()/ffi_call_go() emit extern refs to ffi_call_efi64 /
# ffi_call_go_efi64 (plus ffi_prep_cif_machdep_efi64, _closure_loc_efi64,
# _prep_go_closure_efi64) — all defined in ffiw64.c via the EFI64() macro,
# which on non-X86_WIN64 expands to FFI_HIDDEN name##_efi64 (win64.S gives
# the asm backend ffi_call_win64/ffi_closure_win64/ffi_go_closure_win64).
# FFI_DEFAULT_ABI is FFI_UNIX64 here (ffitarget.h:103), so the runtime
# `if (abi == FFI_EFI64 || FFI_GNUW64)` branches are dead — but the link-time
# symbol refs must still resolve, same story as tramp.c. Symbols are
# FFI_HIDDEN so they don't pollute the exported ABI.
add_third_party_lib(ffi_so
    SOURCES ${LIBFFI_SOURCES}
    C
    OUTPUT_NAME ffi
    SO_LINK_LIBS c
    FLAGS "-fvisibility=default -O2"
    INCLUDE_DIRS ${LIBFFI_INCLUDE_DIRS}
    GEN_HEADERS ${CMAKE_BINARY_DIR}/fficonfig.h
                ${CMAKE_BINARY_DIR}/ffi.h
                ${CMAKE_BINARY_DIR}/ffitarget.h
)

# ===================== libexpat.so =====================
#
# expat_config.h: hand-written, mirrors build_script/libffi/fficonfig.h approach
configure_file(${CMAKE_SOURCE_DIR}/build_script/libexpat/expat_config.h
               ${CMAKE_BINARY_DIR}/expat_config.h COPYONLY)

set(LIBEXPAT_DIR ${CMAKE_SOURCE_DIR}/third_party/libexpat/expat)
set(LIBEXPAT_SOURCES
    ${LIBEXPAT_DIR}/lib/xmlparse.c
    ${LIBEXPAT_DIR}/lib/xmlrole.c
    ${LIBEXPAT_DIR}/lib/xmltok.c
    ${LIBEXPAT_DIR}/lib/random_dev_urandom.c
)
set(LIBEXPAT_INCLUDE_DIRS
    ${LIBEXPAT_DIR}/lib              # <expat.h> <internal.h> 等
    ${CMAKE_BINARY_DIR}              # <expat_config.h>
)

# expat.so (shared). expat.h/expat_external.h use XML_ENABLE_VISIBILITY + -fvisibility=hidden
# to restrict exports to XMLPARSEAPI functions only. Unlike ffi_so, we don't force
# -fvisibility=default — let the library's own visibility annotations work.
# XML_DEV_URANDOM is defined in expat_config.h and pulls in random_dev_urandom.c
# for entropy (opens /dev/urandom, which our kernel provides via devtmpfs).
add_third_party_lib(expat_so
    SOURCES ${LIBEXPAT_SOURCES}
    C
    OUTPUT_NAME expat
    SO_LINK_LIBS c
    FLAGS "-O2 -DXML_ENABLE_VISIBILITY=1"
    INCLUDE_DIRS ${LIBEXPAT_INCLUDE_DIRS}
    GEN_HEADERS ${CMAKE_BINARY_DIR}/expat_config.h
)
