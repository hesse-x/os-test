# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/ — userspace libc + test programs

# libc source list (dual artifacts from same source: libc.a + libc.so)
# start.cc removed (_start moved to crt0.o, see plan_ld2b3 T4/T21)
# start_main.cc removed (stdlib.md): musl src/env/__libc_start_main.c now
# provides __libc_start_main + __init_libc; atexit/__libc_run_atexit, the env
# set (environ/getenv/setenv/putenv/unsetenv/clearenv), and .init/.fini-array
# handling all come from musl upstream (src/env + src/exit). strtol.c removed
# (stdlib.md): musl src/stdlib/strtol.c + strtod.c + atoi/atol/atoll.c cover
# the whole strto* / ato* family. stdio_exit.c provides the strong __stdio_exit
# that overrides musl exit.c's weak dummy (repo FILE isn't on musl's ofl list).
set(LIBC_SOURCES
    lib/stdio.cc lib/malloc.cc
    lib/unistd.cc lib/time.cc lib/file.cc
    lib/ctype.c lib/stdlib_misc.c lib/uname.c
    lib/assert.c lib/signal.cc lib/getrandom.c
    lib/musl_missing.c
    lib/musl_glue.c
    lib/setjmp.S
    lib/stdio_exit.c
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

# ===================== musl socket integration (wayland §1.5) =====================
# Build the upstream musl src/network socket-syscall wrappers into libc, mirroring
# musl_fcntl_objs (same musl-internal include order, same dual -fno-pie/-fPIC build).
# These are NOT a network stack — each is a 6-30 line syscall passthrough. On x86_64
# (SYSCALL_USE_SOCKETCALL undefined) musl expands socketcall_cp(recvmsg,...) →
# syscall_cp(SYS_recvmsg, ...) → direct __NR_recvmsg, NOT the i386 socketcall(2)
# multiplex. The actual socket semantics live in the kernel's AF_UNIX SOCK_STREAM +
# SCM_RIGHTS impl (socket.c); wayland only uses AF_UNIX domain sockets, never TCP/IP,
# so no network stack is required. syscall_cp (cancellation point) is provided by
# musl_pthread.
#
# NOT globbed on purpose: src/network/ also holds the DNS resolver (getaddrinfo/
# resolv/res_msend/dns/lookup_*) which needs /etc/resolv.conf + a real network
# stack and would drag in undefined symbols. Only the pure socket-syscall wrappers
# are listed explicitly — same discipline as musl_fcntl_objs.
#
# Excluded from this list (supplied elsewhere):
#   accept4.c — hand-written wrapper in user/lib/sys_socket.cc (commit e3e5e16),
#              already exported; musl's would multi-define.
#   recv.c was previously excluded for a name collision with the microkernel IPC
#   `recv` primitive, but that IPC symbol has been renamed to `ipc_recv` (see
#   user/include/sys/ipc.h), freeing the POSIX `recv` name — musl recv.c is now
#   included.
set(MUSL_SOCKET_SOURCES
    ${MUSL_DIR}/src/network/socket.c
    ${MUSL_DIR}/src/network/socketpair.c
    ${MUSL_DIR}/src/network/accept.c
    ${MUSL_DIR}/src/network/connect.c
    ${MUSL_DIR}/src/network/bind.c
    ${MUSL_DIR}/src/network/listen.c
    ${MUSL_DIR}/src/network/shutdown.c
    ${MUSL_DIR}/src/network/getsockname.c
    ${MUSL_DIR}/src/network/getpeername.c
    ${MUSL_DIR}/src/network/getsockopt.c
    ${MUSL_DIR}/src/network/setsockopt.c
    ${MUSL_DIR}/src/network/send.c
    ${MUSL_DIR}/src/network/recv.c
    ${MUSL_DIR}/src/network/sendto.c
    ${MUSL_DIR}/src/network/recvfrom.c
    ${MUSL_DIR}/src/network/sendmsg.c
    ${MUSL_DIR}/src/network/recvmsg.c)

add_library(musl_socket_objs OBJECT ${MUSL_SOCKET_SOURCES})
target_include_directories(musl_socket_objs PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_socket_objs PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -fno-pie -Wno-all)

# libc.so needs PIC objects (mirror the libc.a(-fno-pie)/libc.so(-fPIC) dual build).
add_library(musl_socket_objs_so OBJECT ${MUSL_SOCKET_SOURCES})
target_include_directories(musl_socket_objs_so PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_socket_objs_so PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -fPIC -Wno-all)

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
# consumer uses. versionsort.c IS included; its only dep strverscmp is now
# supplied by the musl_string_objs glob (src/string/*.c, see string.md) — no
# separate strverscmp.c here, or both tiers multi-define strverscmp.
# user/include/dirent.h does not declare alphasort.
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
    ${MUSL_DIR}/src/dirent/versionsort.c)

add_library(musl_dirent_objs OBJECT ${MUSL_DIRENT_SOURCES})
target_include_directories(musl_dirent_objs PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_dirent_objs PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -fno-pie -Wno-all -Wno-ignored-attributes)

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
    -m64 ${USER_FREESTANDING_FLAGS} -fPIC -Wno-all -Wno-ignored-attributes)

# ===================== musl mman integration (musl_worklist) =====================
# Build the upstream musl src/mman/*.c into libc, mirroring musl_fcntl_objs
# (same musl-internal include order, same dual -fno-pie/-fPIC build). These
# replace the repo's hand-written mmap/munmap/mprotect (sys_process.cc) and
# mremap/shm_unlink (musl_missing.c), which are deleted this batch.
#
# Every file here is a pure syscall passthrough (1-30 lines): mmap → SYS_mmap(9),
# munmap → SYS_munmap(11), mprotect → SYS_mprotect(10) (musl page-rounds both
# ends before the call; the kernel rejects only non-page-aligned addr, so the
# aligned value musl hands it is accepted), mremap → SYS_mremap(25), msync →
# SYS_msync(26), madvise/posix_madvise → SYS_madvise(28), mincore → SYS_mincore(27),
# mlock/munlock/mlockall/munlockall → SYS_mlock(149)/.../. musl mmap.c's
# weak_alias(__mmap,mmap) supplies the real __mmap/__munmap/__mprotect/__mremap
# the already-migrated musl pthread/malloc/locale/time reach for (their
# forward-declared internal refs); the prior hidden forwarders in musl_glue.c
# are deleted this batch to avoid a duplicate-definition clash.
#
# msync.c uses syscall_cp (cancellation point); __syscall_cp is provided by
# musl_pthread at link time — no shim needed here (same as musl_fcntl_objs).
# posix_madvise.c returns the raw (positive) errno from -__syscall; that is musl
# upstream behaviour, not our concern.
#
# EXCLUDED:
#   memfd_create — musl src/mman has no memfd_create (it lives in musl
#     src/linux/); the repo's hand-written wrapper in sys_process.cc is RETAINED
#     (routes to SYS_MEMFD_CREATE=319). Adding musl/src/linux/memfd_create.c
#     would pull the whole src/linux glob; the one wrapper is cheaper.
#   remap_file_pages — musl mremap.c does NOT define it (it is in src/linux/);
#     not adopted.
#
# msync/madvise/mlock/mlockall/munlock/mincore have NO kernel handler (sys_msync
# etc. are absent from kernel/bsd/syscall.c). The syscall dispatch returns
# -ENOSYS for unregistered numbers, so the musl wrappers expose the symbols
# (callers link) but return -1/ENOSYS — the same surface glibc/Linux would give
# before the feature existed. This is the documented "POSIX symbol present,
# kernel feature absent" middle state (doc/design/todo.md).
set(MUSL_MMAN_SOURCES
    ${MUSL_DIR}/src/mman/mmap.c
    ${MUSL_DIR}/src/mman/munmap.c
    ${MUSL_DIR}/src/mman/mprotect.c
    ${MUSL_DIR}/src/mman/mremap.c
    ${MUSL_DIR}/src/mman/msync.c
    ${MUSL_DIR}/src/mman/madvise.c
    ${MUSL_DIR}/src/mman/posix_madvise.c
    ${MUSL_DIR}/src/mman/mincore.c
    ${MUSL_DIR}/src/mman/mlock.c
    ${MUSL_DIR}/src/mman/munlock.c
    ${MUSL_DIR}/src/mman/mlockall.c
    ${MUSL_DIR}/src/mman/munlockall.c
    ${MUSL_DIR}/src/mman/shm_open.c)

add_library(musl_mman_objs OBJECT ${MUSL_MMAN_SOURCES})
target_include_directories(musl_mman_objs PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_mman_objs PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -fno-pie -Wno-all -Wno-ignored-attributes)

# libc.so PIC mirror (same as musl_fcntl_objs_so above).
add_library(musl_mman_objs_so OBJECT ${MUSL_MMAN_SOURCES})
target_include_directories(musl_mman_objs_so PRIVATE
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_mman_objs_so PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -fPIC -Wno-all -Wno-ignored-attributes)

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
    # BY pthread_create.c itself (src/thread/pthread_create.c:175-177), so
    # init_file_lock(NULL) is a no-op (pthread_create.c:179-182) and the ofl
    # walk iterates an empty list. ofl.c is compiled only to PROVIDE
    # __ofl_lock/__ofl_unlock so the dynamic linker stops failing libc.so
    # relocation with "unresolved PLT symbol __ofl_unlock"; at runtime it does
    # nothing. src/stdio/__stdio_exit.c is NOT compiled here (stdlib.md): it
    # defined __stdio_exit + close_file, but exit is now musl's src/exit/exit.c
    # (musl_stdlib_objs) and __stdio_exit is overridden by our strong
    # user/lib/stdio_exit.c (fflush stdout/stderr); the *_used weak aliases it
    # duplicated are already provided by pthread_create.c, so it is dead weight.
    ${CMAKE_SOURCE_DIR}/third_party/musl/src/stdio/ofl.c
    # __lockfile: referenced by musl stdio internals; its deps (__pthread_self,
    # a_cas, __wait, a_store, __wake) are all already in musl_pthread, so it
    # adds no further undefined chain. It operates on musl FILE->lock/waiters,
    # which is fine — our stdio never enters musl's ofl list, so __lockfile is
    # only reachable via musl's __stdio_exit (which we replace); kept as a
    # harmless provider in case other musl internals reference it.
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
# wcs*/wmem* (and wcpcpy/wcpncpy) are excluded — they belong to the wchar tier.
file(GLOB MUSL_STRING_SOURCES ${MUSL_DIR}/src/string/*.c)
list(REMOVE_ITEM MUSL_STRING_SOURCES
    # C versions superseded by the x86_64 .s asm below — keep them out or the
    # link multi-defines memcpy/memmove/memset.
    ${MUSL_DIR}/src/string/memcpy.c
    ${MUSL_DIR}/src/string/memmove.c
    ${MUSL_DIR}/src/string/memset.c
    # 0 callers; locale machinery already introduced by strerror in musl_pthread.
    ${MUSL_DIR}/src/string/strsignal.c
    # Wide-character set — deferred to the wchar migration tier (not this batch).
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
               $<TARGET_OBJECTS:musl_socket_objs>
               $<TARGET_OBJECTS:musl_dl_objs> $<TARGET_OBJECTS:musl_dirent_objs>
               $<TARGET_OBJECTS:musl_pthread>
               $<TARGET_OBJECTS:musl_string_objs>
               $<TARGET_OBJECTS:musl_math_objs>
               $<TARGET_OBJECTS:musl_stdlib_objs>
               $<TARGET_OBJECTS:musl_mman_objs>
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
               $<TARGET_OBJECTS:musl_socket_objs_so>
               $<TARGET_OBJECTS:musl_dl_objs_so> $<TARGET_OBJECTS:musl_dirent_objs_so>
               $<TARGET_OBJECTS:musl_loader_objs> $<TARGET_OBJECTS:musl_pthread>
               $<TARGET_OBJECTS:musl_string_objs>
               $<TARGET_OBJECTS:musl_math_objs>
               $<TARGET_OBJECTS:musl_stdlib_objs>
               $<TARGET_OBJECTS:musl_mman_objs_so>
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

# ===================== musl math integration (libm) =====================
# Build the upstream musl src/math/*.c + x86_64/*.s + fenv/x86_64/fenv.s into
# libm, replacing the hand-written __builtin_* wrappers in the deleted
# user/lib/math/*.c. Mirrors musl_string_objs: one -fPIC OBJECT sub-library
# (add_musl_lib) wired into BOTH the static libm.a (m) and shared libm.so
# (m_so) via EXTRA_OBJS — -fPIC objects link fine into a -no-pie static archive
# too, so a single compile serves both (same as musl_string_objs → c/c_so).
#
# src/math/*.c is one-function-per-file. The x86_64/*.s asm versions REPLACE the
# C ones for 29 symbols (acosl asinl atan2l atanl ceill exp2l expl expm1l fabs
# fabsf fabsl floorl fmodl llrint llrintf llrintl log10l log1pl log2l logl lrint
# lrintf lrintl remainderl rintl sqrt sqrtf sqrtl truncl, plus __invtrigl) —
# compiling both the .c and .s would multi-define those symbols, so the .c
# twins are dropped from the glob (same pattern as MUSL_STRING_SOURCES drops the
# .c that have a .s twin). The asm .s set is added explicitly via GLOB below.
#
# fenv: src/math/{nearbyint,fma,fmaf,fmal,llrint,...}.c #include <fenv.h> and
# call feclearexcept/feraiseexcept/fetestexcept. On x86_64 musl implements the
# entire fenv ABI in one assembly file — src/fenv/x86_64/fenv.s — so pull it in
# to satisfy those references. musl <fenv.h> needs <bits/fenv.h> (published via
# install-headers.sh), already on this target's musl arch include path.
#
# Internal helpers (__fpclassify* __signbit* __math_* __polevll __sinl __cosl
# __tanl __rem_pio2* __expo2* __signgam __lgamma_r ...) are all caught by the
# src/math/__*.c glob and stay hidden (not in libm.map). The only external
# reference math sources make is errno via __errno_location (provided by
# musl_pthread in libc), resolved at program link time.
file(GLOB MUSL_MATH_SOURCES ${MUSL_DIR}/src/math/*.c)
list(REMOVE_ITEM MUSL_MATH_SOURCES
    # C versions superseded by the x86_64 .s asm below — keep them out or the
    # link multi-defines these symbols.
    ${MUSL_DIR}/src/math/acosl.c
    ${MUSL_DIR}/src/math/asinl.c
    ${MUSL_DIR}/src/math/atan2l.c
    ${MUSL_DIR}/src/math/atanl.c
    ${MUSL_DIR}/src/math/ceill.c
    ${MUSL_DIR}/src/math/exp2l.c
    ${MUSL_DIR}/src/math/expl.c
    ${MUSL_DIR}/src/math/expm1l.c
    ${MUSL_DIR}/src/math/fabs.c
    ${MUSL_DIR}/src/math/fabsf.c
    ${MUSL_DIR}/src/math/fabsl.c
    ${MUSL_DIR}/src/math/floorl.c
    ${MUSL_DIR}/src/math/fmodl.c
    ${MUSL_DIR}/src/math/__invtrigl.c
    ${MUSL_DIR}/src/math/llrint.c
    ${MUSL_DIR}/src/math/llrintf.c
    ${MUSL_DIR}/src/math/llrintl.c
    ${MUSL_DIR}/src/math/log10l.c
    ${MUSL_DIR}/src/math/log1pl.c
    ${MUSL_DIR}/src/math/log2l.c
    ${MUSL_DIR}/src/math/logl.c
    ${MUSL_DIR}/src/math/lrint.c
    ${MUSL_DIR}/src/math/lrintf.c
    ${MUSL_DIR}/src/math/lrintl.c
    ${MUSL_DIR}/src/math/remainderl.c
    ${MUSL_DIR}/src/math/rintl.c
    ${MUSL_DIR}/src/math/sqrt.c
    ${MUSL_DIR}/src/math/sqrtf.c
    ${MUSL_DIR}/src/math/sqrtl.c
    ${MUSL_DIR}/src/math/truncl.c)
file(GLOB MUSL_MATH_ASM_SOURCES ${MUSL_DIR}/src/math/x86_64/*.s)
list(APPEND MUSL_MATH_SOURCES
    ${MUSL_MATH_ASM_SOURCES}
    ${MUSL_DIR}/src/fenv/x86_64/fenv.s
    # Generic fenv wrappers NOT provided by x86_64/fenv.s. The .s defines the
    # low-level feclearexcept/feraiseexcept/fetestexcept/fegetenv/fesetenv/
    # fegetround/__fesetround; these generic .c wrap __fesetround/fegetenv/etc.
    # to provide the remaining public fenv API. fenv.c is EXCLUDED — it defines
    # feclearexcept/etc. that the .s already provides (multi-def). __flt_rounds
    # backs the FLT_ROUNDS macro in <float.h>.
    ${MUSL_DIR}/src/fenv/fesetround.c
    ${MUSL_DIR}/src/fenv/fegetexceptflag.c
    ${MUSL_DIR}/src/fenv/fesetexceptflag.c
    ${MUSL_DIR}/src/fenv/feholdexcept.c
    ${MUSL_DIR}/src/fenv/feupdateenv.c
    ${MUSL_DIR}/src/fenv/__flt_rounds.c)
add_musl_lib(musl_math_objs SOURCES ${MUSL_MATH_SOURCES})

# ===================== musl stdlib integration (stdlib.md) =====================
# Build the upstream musl stdlib + prng + env + exit chain into libc, mirroring
# musl_string_objs / musl_math_objs (one -fPIC OBJECT sub-library via add_musl_lib,
# wired into BOTH libc.a and libc.so via EXTRA_OBJS — -fPIC objects link fine
# into a -no-pie static ELF too, so a single compile serves both).
#
# This batch RETIRES the repo's hand-written startup/env/exit shim:
#   user/lib/start_main.cc (deleted)   — __libc_start_main, atexit,
#     __libc_run_atexit, __libc_run_init_array/__libc_run_fini_array,
#     __libc_fini_array_trampoline, decode_auxv, environ/__environ,
#     getenv/setenv/putenv/unsetenv/clearenv, __libc_env_init.
#   user/lib/musl_startup.c (deleted)  — musl_libc_init_aux (set libc.page_size/
#     auxv); redundant now that musl __init_libc sets them.
#   user/lib/stdlib_misc.c (slimmed)   — exit, rand/srand/rand_r,
#     abs/labs/llabs/imaxabs/imaxdiv/div/ldiv/lldiv, qsort/bsearch removed.
#   user/lib/signal.cc (abort deleted) — musl src/exit/abort.c takes over.
#   user/lib/strtol.c (deleted)        — musl strtol.c/strtod.c/atoi/atol/atoll
#     cover the whole strto*/ato* family.
# musl's __libc_start_main (src/env/__libc_start_main.c) drives BOTH paths:
#   static (DYNAMIC=0): __init_libc decodes the kernel pair-form auxv into the
#     flat AT_*-indexed array musl expects, sets __environ/libc.page_size/
#     __hwcap/__sysinfo/__progname, calls __init_tls (mmaps PT_TLS, sets
#     FS_BASE) + __init_ssp (canary); then libc_start_init walks
#     __init_array_start..end. exit() runs __libc_exit_fini (exit.c walks
#     __fini_array_start..end + _fini) — the linker script provides these.
#   dynamic (DYNAMIC=1): the fused loader (__dls3 in dynlink.c) already set
#     __environ/libc.page_size/__hwcap and ran __init_tp for TLS before jumping
#     to the main ELF _start; musl __libc_start_main then calls __init_libc
#     (idempotent — __init_tls is a no-op in dynlink.c, __init_ssp re-sets the
#     canary) and __libc_start_init = dynlink do_init_fini(tail) over every
#     DSO's .init_array. exit() runs __libc_exit_fini = dynlink's fini walk.
# The kernel-built auxv (kernel/bsd/proc.c execve / proc_create.c) provides
# AT_PHDR/PHENT/PHNUM/ENTRY/PAGESZ/RANDOM/EXECFN + AT_UID==AT_EUID==AT_GID==
# AT_EGID==0, AT_SECURE=0, so __init_libc's secure-check returns early (no
# /dev/null open) and getenv("LD_LIBRARY_PATH") works in the loader.
#
# EXCLUDED from this batch (kept as repo implementations, tracked in todo.md):
#   src/misc/realpath.c   — needs /proc/self/fd/N + readlink + O_PATH (no
#     procfs); repo stdlib_misc.c:realpath (getcwd + lexical collapse) kept.
#   src/temp/{mkstemp,__randname,...}.c — __randname needs __clock_gettime
#     (time module not yet migrated; would clash with repo time.cc); repo
#     stdlib_misc.c:mkstemp/mktemp (getpid-based) kept.
#   src/conf/sysconf.c + src/legacy/getpagesize.c — musl sysconf redefines
#     _SC_NPROCESSORS_ONLN semantics; repo stdlib_misc.c (sys_sysconf-backed)
#     kept.
#   src/stdlib/{wcstod,wcstol}.c — wchar tier (not migrated).
#   src/stdlib/{ecvt,fcvt,gcvt}.c — deprecated BSD, 0 callers, sprintf chain.
#   src/env/{__init_tls,__stack_chk_fail,__reset_tls}.c — already in
#     musl_pthread; compiling here would multi-define.
#   src/exit/assert.c — repo lib/assert.c keeps __assert_fail.
#   src/exit/_Exit.c — already in musl_unistd_objs (+ _so); would multi-def _Exit.
#   src/prng/{random,drand48,...}.c — 0 callers (rand/rand_r suffice).
set(MUSL_STDLIB_SOURCES
    # pure compute
    ${MUSL_DIR}/src/stdlib/abs.c
    ${MUSL_DIR}/src/stdlib/labs.c
    ${MUSL_DIR}/src/stdlib/llabs.c
    ${MUSL_DIR}/src/stdlib/imaxabs.c
    ${MUSL_DIR}/src/stdlib/imaxdiv.c
    ${MUSL_DIR}/src/stdlib/div.c
    ${MUSL_DIR}/src/stdlib/ldiv.c
    ${MUSL_DIR}/src/stdlib/lldiv.c
    ${MUSL_DIR}/src/stdlib/atoi.c
    ${MUSL_DIR}/src/stdlib/atol.c
    ${MUSL_DIR}/src/stdlib/atoll.c
    # one file provides strtol/strtoul/strtoll/strtoull/strtoimax/strtoumax +
    # the __strtol_internal weak aliases.
    ${MUSL_DIR}/src/stdlib/strtol.c
    # one file provides strtod/strtof/strtold + _l weak aliases (deps on
    # floatscan + shgetc, both in src/internal, + libm already merged in libc).
    ${MUSL_DIR}/src/stdlib/strtod.c
    ${MUSL_DIR}/src/stdlib/atof.c
    ${MUSL_DIR}/src/stdlib/bsearch.c
    ${MUSL_DIR}/src/stdlib/qsort.c
    # prng: musl's LCG (31-bit, RAND_MAX=0x7fffffff) replaces the repo's
    # 15-bit LCG. random/drand48 excluded (0 callers).
    ${MUSL_DIR}/src/prng/rand.c
    ${MUSL_DIR}/src/prng/rand_r.c
    # strtod/strtol scanner deps (src/internal, pure compute — __floatscan uses
    # only libm + errno; __shgetc fallback-references __uflow, which the stub
    # below satisfies and is never reached at runtime for string scanning).
    ${MUSL_DIR}/src/internal/intscan.c
    ${MUSL_DIR}/src/internal/shgetc.c
    ${MUSL_DIR}/src/internal/floatscan.c
    # startup + env + exit chain (the shim retirement core)
    ${MUSL_DIR}/src/env/__libc_start_main.c
    ${MUSL_DIR}/src/env/__environ.c
    ${MUSL_DIR}/src/env/getenv.c
    ${MUSL_DIR}/src/env/setenv.c
    ${MUSL_DIR}/src/env/putenv.c
    ${MUSL_DIR}/src/env/unsetenv.c
    ${MUSL_DIR}/src/env/clearenv.c
    # _Exit.c intentionally NOT here (already in musl_unistd_objs/_so).
    ${MUSL_DIR}/src/exit/exit.c
    ${MUSL_DIR}/src/exit/atexit.c
    ${MUSL_DIR}/src/exit/abort.c
    ${MUSL_DIR}/src/exit/quick_exit.c
    ${MUSL_DIR}/src/exit/at_quick_exit.c
    # __uflow stub — satisfies the link-time reference musl's shgetc.c emits;
    # unreachable at runtime (see user/lib/musl_shim/scan_uflow_stub.c).
    ${CMAKE_SOURCE_DIR}/user/lib/musl_shim/scan_uflow_stub.c)
add_musl_lib(musl_stdlib_objs SOURCES ${MUSL_STDLIB_SOURCES})

# ===================== musl single-libc philosophy =====================
# Real math (musl src/math + src/fenv) is merged INTO libc (c + c_so) via
# EXTRA_OBJS below — every static ELF gets math from libc.a, every dynamic
# ELF/.so gets it from libc.so, with zero -lm bookkeeping. libm.so is kept
# only as an EMPTY stub .so (see m_so below) so legacy `-lm` links succeed
# (the build-gcc / Mesa milestones pass -lm and expect libm.so to exist); it
# contributes no symbols, math resolves from libc.so at runtime. Same pattern
# glibc uses after merging libm/libpthread/libdl into libc. libpthread.so stub
# can be added the same way if a `-lpthread` consumer appears (pthread is
# already in libc via musl_pthread).

# libm.so — empty stub ELF. No SOURCES/EXTRA_OBJS → the link emits a .so with
# only a SONAME and no dynsym exports. Shipped to lib/libm.so so -lm resolves
# at link time; runtime math comes from libc.so. (If a future -lm-static
# consumer appears, add an empty static `m` stub analogously; none exists now.)
add_user_lib(m_so
    SOURCES lib/libm_stub.c
    C
    FLAGS "-fPIC -fvisibility=default"
    SHARED
    OUTPUT_NAME m
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
# mathsink — musl libm end-to-end smoke (sqrt/sin/exp/copysignl/fmodl/sincos).
# Temporary diagnostic; ships to /usr/bin so the shell can run it.
add_user_elf(mathsink C SOURCES test/mathsink.c LINK_LIBS c IMAGE_PATH usr/bin/mathsink)
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
    "-Wno-unused-parameter -Wno-duplicate-decl-specifier -Wno-implicit-function-declaration -Wno-return-type -Wno-int-conversion -Wno-format -Wno-implicit-fallthrough -Wno-sign-compare -Wno-error=cpp -Wno-maybe-uninitialized -Wno-sometimes-uninitialized -Wno-unknown-warning-option -Wno-deprecated-non-prototype")

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
            FLAGS "-Wno-unused-parameter -Wno-format -D__linux__ -include ${CMAKE_BINARY_DIR}/libdrm_config.h -Wno-unknown-warning-option -Wno-deprecated-non-prototype -Wno-macro-redefined")
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
    FLAGS "-Wno-unused-parameter -Wno-format -D__linux__ -include ${CMAKE_BINARY_DIR}/libdrm_config.h -Wno-macro-redefined"
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

# ===================== wayland (third_party/wayland, 1.25.91) =====================
#
# Build libwayland-server.so / libwayland-client.so via add_third_party_lib(SHARED),
# no meson. See wayland.md. Two build-time roles:
#   - wayland-scanner: a HOST tool (runs on the dev machine during ./build.sh) that
#     parses protocol/wayland.xml → wayland-protocol.c + 4 *-protocol.h. Built with
#     the host cc + host glibc + vendored expat (dev box has no system expat header).
#   - libwayland-*.so: freestanding target libs, built by add_third_party_lib.
#
# §1.5 of wayland_worklist.md is obsolete: libc.so already exports
# readv/writev/sendmsg/recvmsg/socketpair/posix_fallocate/memfd_create/accept4/flock/
# mremap/strndup/gettid/prctl and the kernel implements sys_flock/sys_mremap, so no
# libc wrappers or syscall gaps remain — only the build adapter (this section).

# ---- 1. config.h + wayland-version.h (consumed by the freestanding .so) ----
# config.h template lives in the main repo (build_script/wayland/config.h), NOT in
# the wayland submodule working tree — mirrors libdrm (config.h in build_script/libdrm).
# Copied to build/wayland_config.h and force-injected via -include (see FLAGS below).
#
# Three sources have EXPLICIT config includes that -include cannot satisfy (the
# preprocessor still tries to resolve the literal #include):
#   wayland-os.c:28   #include "../config.h"   (src/ → third_party/wayland/config.h)
#   connection.c:29   #include "../config.h"   (same)
#   wayland-shm.c:33  #include "config.h"      (via -I third_party/wayland)
# ../config.h resolves relative to src/ into the submodule tree, which we refuse to
# pollute (third_party/wayland is a submodule; meson writes config.h there but we
# don't). Instead we copy those 3 sources to build/wayland_src/ at build time and
# rewrite the include to "wayland_config.h", resolved via -I build. The other .so
# sources compile straight from the submodule untouched. The -include remains as a
# belt-and-suspenders injection for any TU that lacks an explicit include.
configure_file(${CMAKE_SOURCE_DIR}/build_script/wayland/config.h
               ${CMAKE_BINARY_DIR}/wayland_config.h COPYONLY)

# Rewrite the 3 explicit config includes → "wayland_config.h" in build/ copies.
# Both "../config.h" and "config.h" spellings are swapped; only the 3 affected
# files are listed so the rest of the submodule compiles verbatim. We use python3
# (already a build dep via musl_rules) rather than sed: under CMake VERBATIM the
# '#' in "#include" gets mis-parsed by the ninja command line, and sed's quoting
# is fragile. A tiny read/replace/write is unambiguous.
set(WAYLAND_SRC_DIR ${CMAKE_SOURCE_DIR}/third_party/wayland/src)
set(WAYLAND_REWRITE_OUTPUTS
    ${CMAKE_BINARY_DIR}/wayland_src/connection.c
    ${CMAKE_BINARY_DIR}/wayland_src/wayland-os.c
    ${CMAKE_BINARY_DIR}/wayland_src/wayland-shm.c
    ${CMAKE_BINARY_DIR}/wayland_src/wayland-cursor.c
    ${CMAKE_BINARY_DIR}/wayland_src/os-compatibility.c)
set(WAYLAND_CURSOR_DIR ${CMAKE_SOURCE_DIR}/third_party/wayland/cursor)
set(WAYLAND_REWRITE_SCRIPT ${CMAKE_BINARY_DIR}/wayland_rewrite_includes.py)
file(WRITE ${WAYLAND_REWRITE_SCRIPT}
"import sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src).read()
data = data.replace('#include \"../config.h\"', '#include \"wayland_config.h\"')
data = data.replace('#include \"config.h\"', '#include \"wayland_config.h\"')
open(dst, 'w').write(data)
")
add_custom_command(
    OUTPUT ${WAYLAND_REWRITE_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/wayland_src
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_SRC_DIR}/connection.c
                ${CMAKE_BINARY_DIR}/wayland_src/connection.c
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_SRC_DIR}/wayland-os.c
                ${CMAKE_BINARY_DIR}/wayland_src/wayland-os.c
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_SRC_DIR}/wayland-shm.c
                ${CMAKE_BINARY_DIR}/wayland_src/wayland-shm.c
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_CURSOR_DIR}/wayland-cursor.c
                ${CMAKE_BINARY_DIR}/wayland_src/wayland-cursor.c
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_CURSOR_DIR}/os-compatibility.c
                ${CMAKE_BINARY_DIR}/wayland_src/os-compatibility.c
    DEPENDS ${WAYLAND_SRC_DIR}/connection.c
            ${WAYLAND_SRC_DIR}/wayland-os.c
            ${WAYLAND_SRC_DIR}/wayland-shm.c
            ${WAYLAND_CURSOR_DIR}/wayland-cursor.c
            ${WAYLAND_CURSOR_DIR}/os-compatibility.c
            ${CMAKE_BINARY_DIR}/wayland_config.h
    COMMENT "Rewriting wayland config.h includes"
    VERBATIM)
add_custom_target(wayland_rewrite_src DEPENDS ${WAYLAND_REWRITE_OUTPUTS})

# wayland-version.h: @-substituted from src/wayland-version.h.in. Included by
# scanner.c:28 (host) and wayland-server-core.h:33 / wayland-client-core.h:31
# (freestanding .so), so it must resolve from both — generate into build/ and -I it
# from both sides. Wrapped in a function so the short VERSION name stays local
# (same discipline as _libffi_generate_ffi_header above).
function(_wayland_generate_version_header)
    set(WAYLAND_VERSION "1.25.91")
    set(WAYLAND_VERSION_MAJOR 1)
    set(WAYLAND_VERSION_MINOR 25)
    set(WAYLAND_VERSION_MICRO 91)
    configure_file(${CMAKE_SOURCE_DIR}/third_party/wayland/src/wayland-version.h.in
                   ${CMAKE_BINARY_DIR}/wayland-version.h)
endfunction()
_wayland_generate_version_header()

# ---- 2. host wayland-scanner (build-time tool) ----
# Built with the bare host cc (NO freestanding flags / NO _tp_base_compile_flags) so
# it links against the host glibc and runs on the dev machine. scanner.c needs only
# wayland-version.h (WAYLAND_VERSION) + expat for XML parsing. We deliberately do NOT
# define HAVE_LIBXML (skips DTD validation + the #include "wayland.dtd.h" at
# scanner.c:41-46, so embed.py/wayland.dtd are unused) and do NOT define HAVE_STRNDUP
# (host glibc has strndup anyway; the #ifndef fallback at scanner.c:1031 is inert).
# expat: the dev box has no system expat.h, so recompile the vendored
# third_party/libexpat sources for the host and link them in directly. -w silences
# expat's ~6k-line warnings (host build, not gated by -Werror).
# Entropy: the shared build/expat_config.h defines XML_DEV_URANDOM, which makes
# xmlparse.c call writeRandomBytes_dev_urandom (supplied by random_dev_urandom.c).
# For the host scanner we use a SEPARATE host expat config (build_script/libexpat/
# expat_config_host.h → build/wayland_host/expat_config.h) that defines
# HAVE_GETRANDOM instead, and compile random_getrandom.c (not random_dev_urandom.c)
# — host glibc >=2.25 provides getrandom(2). Codegen needs no cryptographic
# entropy; this only seeds expat's hash randomization. The host expat objects put
# -I build/wayland_host ahead of -I build so "expat_config.h" resolves to the host
# copy, overriding the shared freestanding one.
configure_file(${CMAKE_SOURCE_DIR}/build_script/libexpat/expat_config_host.h
               ${CMAKE_BINARY_DIR}/wayland_host/expat_config.h COPYONLY)
set(WAYLAND_SCANNER_HOST_FLAGS
    -O2 -w
    -I${WAYLAND_SRC_DIR}
    -I${CMAKE_SOURCE_DIR}/third_party/wayland
    -I${CMAKE_BINARY_DIR}
    -I${LIBEXPAT_DIR}/lib)
# expat objects get the host config dir first so "expat_config.h" hits the host copy.
set(WAYLAND_SCANNER_HOST_EXPAT_FLAGS
    -O2 -w
    -I${WAYLAND_SRC_DIR}
    -I${CMAKE_SOURCE_DIR}/third_party/wayland
    -I${CMAKE_BINARY_DIR}/wayland_host
    -I${CMAKE_BINARY_DIR}
    -I${LIBEXPAT_DIR}/lib)
set(WAYLAND_SCANNER_HOST_OBJS
    ${CMAKE_BINARY_DIR}/wayland_scanner_scanner.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_util.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_xmlparse.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_xmlrole.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_xmltok.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_getrandom.o)
add_custom_command(
    OUTPUT ${WAYLAND_SCANNER_HOST_OBJS}
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_FLAGS} -c
                ${WAYLAND_SRC_DIR}/scanner.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_scanner.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_FLAGS} -c
                ${WAYLAND_SRC_DIR}/wayland-util.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_util.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_EXPAT_FLAGS} -c
                ${LIBEXPAT_DIR}/lib/xmlparse.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_xmlparse.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_EXPAT_FLAGS} -c
                ${LIBEXPAT_DIR}/lib/xmlrole.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_xmlrole.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_EXPAT_FLAGS} -c
                ${LIBEXPAT_DIR}/lib/xmltok.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_xmltok.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_EXPAT_FLAGS} -c
                ${LIBEXPAT_DIR}/lib/random_getrandom.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_getrandom.o
    DEPENDS ${WAYLAND_SRC_DIR}/scanner.c
            ${WAYLAND_SRC_DIR}/wayland-util.c
            ${LIBEXPAT_DIR}/lib/xmlparse.c
            ${LIBEXPAT_DIR}/lib/xmlrole.c
            ${LIBEXPAT_DIR}/lib/xmltok.c
            ${LIBEXPAT_DIR}/lib/random_getrandom.c
            ${CMAKE_BINARY_DIR}/wayland-version.h
            ${CMAKE_BINARY_DIR}/wayland_host/expat_config.h
    COMMENT "Compiling host wayland-scanner objects"
    VERBATIM)
# wayland-util.c uses round() (wl_fixed_from_double) → -lm on the host link.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/wayland-scanner
    COMMAND ${CMAKE_C_COMPILER} -o ${CMAKE_BINARY_DIR}/wayland-scanner
                ${WAYLAND_SCANNER_HOST_OBJS} -lm
    DEPENDS ${WAYLAND_SCANNER_HOST_OBJS}
    COMMENT "Linking host wayland-scanner")
add_custom_target(wayland_scanner_host DEPENDS ${CMAKE_BINARY_DIR}/wayland-scanner)

# ---- 3. generate the 5 protocol files from wayland.xml ----
set(WL_XML ${CMAKE_SOURCE_DIR}/third_party/wayland/protocol/wayland.xml)
set(WL_PROTOCOL_OUTPUTS
    ${CMAKE_BINARY_DIR}/wayland-server-protocol.h
    ${CMAKE_BINARY_DIR}/wayland-server-protocol-core.h
    ${CMAKE_BINARY_DIR}/wayland-client-protocol.h
    ${CMAKE_BINARY_DIR}/wayland-client-protocol-core.h
    ${CMAKE_BINARY_DIR}/wayland-protocol.c)
add_custom_command(
    OUTPUT ${WL_PROTOCOL_OUTPUTS}
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner server-header
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-server-protocol.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner server-header -c
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-server-protocol-core.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner client-header
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-client-protocol.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner client-header -c
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-client-protocol-core.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner public-code
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-protocol.c
    DEPENDS wayland_scanner_host ${WL_XML}
    COMMENT "Generating wayland protocol sources"
    VERBATIM)
add_custom_target(wayland_protocol ALL DEPENDS ${WL_PROTOCOL_OUTPUTS})

# ---- 4. freestanding .so ----
# Generated headers all land in build/ so the .so sources resolve them via -I build
# (wayland-server.h:109 #include "wayland-server-protocol.h", wayland-client.h:40 likewise).
# -include wayland_config.h forces config.h into every TU (see step 1). Visibility:
# wayland-util.h:45 defines WL_EXPORT as visibility("default"); add_third_party_lib's
# SHARED default -fvisibility=hidden exports only WL_EXPORT symbols — matches upstream.
# SO_LINK_LIBS ffi c → DT_NEEDED libffi.so (ffi closures, event-loop.c) + libc.so;
# resolves to the ffi_so / c_so targets (third_party_rules.cmake SO_LINK_LIBS loop).
set(WL_GEN_HEADERS
    ${CMAKE_BINARY_DIR}/wayland-server-protocol.h
    ${CMAKE_BINARY_DIR}/wayland-server-protocol-core.h
    ${CMAKE_BINARY_DIR}/wayland-client-protocol.h
    ${CMAKE_BINARY_DIR}/wayland-client-protocol-core.h
    ${CMAKE_BINARY_DIR}/wayland-version.h
    ${CMAKE_BINARY_DIR}/wayland_config.h)

# connection.c / wayland-os.c / wayland-shm.c come from build/wayland_src/ (the
# sed-rewritten copies with config includes pointed at wayland_config.h); the rest
# compile verbatim from the submodule. add_dependencies(wayland_rewrite_src) below
# guarantees the rewritten copies exist before the .so objects compile.
add_third_party_lib(wayland_server_so
    SOURCES ${WAYLAND_SRC_DIR}/wayland-server.c
            ${CMAKE_BINARY_DIR}/wayland_src/wayland-shm.c
            ${WAYLAND_SRC_DIR}/event-loop.c
            ${CMAKE_BINARY_DIR}/wayland_src/connection.c
            ${CMAKE_BINARY_DIR}/wayland_src/wayland-os.c
            ${WAYLAND_SRC_DIR}/wayland-util.c
            ${CMAKE_BINARY_DIR}/wayland-protocol.c
    C
    OUTPUT_NAME wayland-server
    SO_LINK_LIBS ffi c
    INCLUDE_DIRS ${WAYLAND_SRC_DIR}
                 ${CMAKE_SOURCE_DIR}/third_party/wayland
                 ${CMAKE_BINARY_DIR}
    FLAGS "-include ${CMAKE_BINARY_DIR}/wayland_config.h"
    GEN_HEADERS ${WL_GEN_HEADERS})
add_dependencies(wayland_server_so wayland_protocol wayland_rewrite_src)

add_third_party_lib(wayland_client_so
    SOURCES ${WAYLAND_SRC_DIR}/wayland-client.c
            ${CMAKE_BINARY_DIR}/wayland_src/connection.c
            ${CMAKE_BINARY_DIR}/wayland_src/wayland-os.c
            ${WAYLAND_SRC_DIR}/wayland-util.c
            ${CMAKE_BINARY_DIR}/wayland-protocol.c
    C
    OUTPUT_NAME wayland-client
    SO_LINK_LIBS ffi c
    INCLUDE_DIRS ${WAYLAND_SRC_DIR}
                 ${CMAKE_SOURCE_DIR}/third_party/wayland
                 ${CMAKE_BINARY_DIR}
    FLAGS "-include ${CMAKE_BINARY_DIR}/wayland_config.h"
    GEN_HEADERS ${WL_GEN_HEADERS})
add_dependencies(wayland_client_so wayland_protocol wayland_rewrite_src)

# ---- 5. libwayland-cursor.so (第三个 wayland target) ----
# cursor/ 下 wayland-cursor.c / os-compatibility.c 也 #include "config.h"（相对当前
# 目录解析），而 third_party/wayland/config.h 不存在（项目用 build/wayland_config.h
# + -include 注入替代），故二者同样走 rewrite（WAYLAND_REWRITE_OUTPUTS 已含之）；
# xcursor.c 不 include config.h，verbatim 编译。cursor 调用 wl_shm_* / wl_proxy_*
# 等客户端协议符号，故 SO_LINK_LIBS 拉入 wayland-client（文件级 DEPENDS 自动牵
# build/libwayland-client.so，add_dependencies 再补 target 保险）。INCLUDE_DIRS 多
# 加 cursor 目录以解析 "xcursor.h" / "os-compatibility.h"。运行时基本光标走内置
# load_fallback_theme（cursor-data.h，无需磁盘主题）；完整主题需向根分区放 cursor
# theme 数据并传 ICONDIR / XCURSOR_PATH（见 doc/design/todo.md）。
add_third_party_lib(wayland_cursor_so
    SOURCES ${CMAKE_BINARY_DIR}/wayland_src/wayland-cursor.c
            ${CMAKE_BINARY_DIR}/wayland_src/os-compatibility.c
            ${WAYLAND_CURSOR_DIR}/xcursor.c
    C
    OUTPUT_NAME wayland-cursor
    SO_LINK_LIBS wayland-client ffi c
    INCLUDE_DIRS ${WAYLAND_SRC_DIR}
                 ${CMAKE_SOURCE_DIR}/third_party/wayland
                 ${WAYLAND_CURSOR_DIR}
                 ${CMAKE_BINARY_DIR}
    FLAGS "-include ${CMAKE_BINARY_DIR}/wayland_config.h"
    GEN_HEADERS ${WL_GEN_HEADERS})
add_dependencies(wayland_cursor_so wayland_protocol wayland_rewrite_src wayland_client_so)
