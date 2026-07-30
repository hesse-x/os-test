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
# the whole strto* / ato* family. stdio.cc + stdio_exit.c removed (stdio.md):
# musl src/stdio/*.c (musl_stdio_objs) now provides the full stdio family with
# musl's struct _IO_FILE; musl's own __stdio_exit.c (globbed in) walks the ofl
# list + __stdin_used/__stdout_used/__stderr_used at exit, so the repo's
# stdout/stderr fflush override (stdio_exit.c) is obsolete. utils/kvformat.c
# removed from here: the only userspace caller was stdio.cc (gone); the kernel
# has its own -D__KERNEL__ -mno-sse compile of the same file (utils/CMakeLists
# .txt deliberately dual-compiles), so it is unaffected and utils/kvformat.{c,h}
# are NOT deleted.
set(LIBC_SOURCES
    lib/malloc.cc
    lib/unistd.cc lib/time.cc lib/file.cc
    lib/ctype.c lib/stdlib_misc.c lib/uname.c
    lib/assert.c lib/signal.cc lib/getrandom.c
    lib/musl_missing.c
    lib/musl_glue.c
    lib/setjmp.S
    lib/io_multiplex.cc
    lib/sys_process.cc
    lib/sys_device.cc
    lib/sys_ipc.cc
    lib/sys_socket.cc
    lib/musl_loader_shim.c
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

# ===================== musl stdio integration (musl_worklist stdio) =====================
# Build the upstream musl src/stdio/*.c into libc via add_musl_lib (single
# -fPIC OBJECT lib, same musl-internal include order). This replaces the
# hand-written user/lib/stdio.cc + user/lib/stdio_exit.c (deleted this batch):
# the repo's custom struct _FILE → musl's struct _IO_FILE (stdio_impl.h), with
# the full printf/scanf/getline/getdelim/open_memstream/fmemopen/... family now
# from upstream. vfprintf.c carries the built-in fmt_fp (long double) float
# printer — %f/%g/%e work (was a TODO in the hand-written version); it links
# libm (already merged). vfscanf.c uses __intscan/__floatscan (already in
# musl_stdlib_objs). vfprintf's narrow path references wctomb; vfscanf's
# references mbrtowc + mbsinit — supplied by the musl_multibyte_objs block
# (narrow-path 5-file subset). The 23 wide-char stdio files (vfwprintf/fgetwc/
# ...) are EXCLUDED — see the REMOVE_ITEM list; they drag isw*/wcsnlen/btowc/
# __c_locale which this OS doesn't compile, causing ld-musl runtime reloc
# failures, and no consumer + no <wchar.h> publication justifies them.
# No UAPI sync step (no xos/stdio.h).
#
# glob + exclude: src/stdio holds 117 files; a handful clash or drag heavy deps
# and are removed. The exclude rationale:
#   ofl.c, __lockfile.c — ALREADY compiled into musl_pthread (ofl.c PROVIDES
#     __ofl_lock/__ofl_unlock; __lockfile.c PROVIDES __lockfile/__unlockfile,
#     whose deps __pthread_self/a_cas/__wait/a_store/__wake are all in
#     musl_pthread). Recompiling here would multi-define. After this migration
#     the ofl list IS real (musl __fdopen/fopen call __ofl_add) and FLOCK IS
#     real (musl stdio paths call __lockfile) — see the updated musl_pthread
#     comment below.
#   rename.c — already in musl_unistd_objs (+ _so); recompiling multi-defines.
#   popen.c, pclose.c — popen needs posix_spawn (the whole src/process spawn
#     chain); pclose is useless without popen. No consumer; excluded.
#   tmpfile.c, tmpnam.c, tempnam.c — need __randname → __clock_gettime; the time
#     module is not yet migrated (same blocker as mkstemp, todo.md:345).
#   dprintf.c, vdprintf.c — NOT excluded. The musl dynamic loader (fused into
#     libc.so via musl_loader_objs) resolves its dprintf/vdprintf to these musl
#     native impls (which route through vfprintf). Every loader call site runs
#     AFTER reloc_all(&ldso) (dynlink.c:1432), so the loader's own PLT is
#     already relocated when vfprintf's PLT is reached — no boot-safe shim
#     needed. dprintf/vdprintf are NOT public libc symbols (hidden under
#     -fvisibility=hidden in the c_so SHARED path; libc.map does not list them).
# ofl_add.c is NOT excluded — it is currently compiled nowhere, and musl
# __fdopen/fopen call __ofl_add to register each FILE on the global open-FILE
# list (which __stdio_exit walks at exit). The glob brings it in.
file(GLOB MUSL_STDIO_SOURCES ${MUSL_DIR}/src/stdio/*.c)
list(REMOVE_ITEM MUSL_STDIO_SOURCES
    ${MUSL_DIR}/src/stdio/ofl.c          # in musl_pthread (PROVIDE __ofl_lock/__ofl_unlock)
    ${MUSL_DIR}/src/stdio/__lockfile.c   # in musl_pthread (PROVIDE __lockfile/__unlockfile)
    ${MUSL_DIR}/src/stdio/rename.c       # in musl_unistd_objs (+ _so)
    ${MUSL_DIR}/src/stdio/popen.c        # needs posix_spawn (src/process chain); no consumer
    ${MUSL_DIR}/src/stdio/pclose.c       # pair with popen; useless without it
    ${MUSL_DIR}/src/stdio/tmpfile.c      # needs __randname → __clock_gettime; time not migrated
    ${MUSL_DIR}/src/stdio/tmpnam.c       # same
    ${MUSL_DIR}/src/stdio/tempnam.c      # same
    # NOTE: dprintf.c/vdprintf.c are NOT excluded — the loader resolves its
    # dprintf/vdprintf to musl's native stdio impl (routes through vfprintf).
    # Every loader call site runs AFTER reloc_all(&ldso) (dynlink.c:1432), so
    # the loader's own PLT is already relocated when vfprintf's PLT is reached.
    # musl_loader_shim.c no longer provides a boot-safe pair.
    # --- wide-char stdio (23 w* files) — EXCLUDED (no consumer + runtime reloc cost)
    # vfwprintf/vfwscanf/fgetwc/fputwc/fputws/fgetws/ungetwc/fwide/fwprintf/fwscanf/
    # wprintf/wscanf/swprintf/swscanf/vwprintf/vwscanf/vswprintf/vswscanf/getwc/getwchar/
    # putwc/putwchar/open_wmemstream. These reference iswspace/iswdigit (wide ctype,
    # src/ctype/isw*.c), wcsnlen (src/string), btowc/wctob (src/multibyte), and the
    # C-locale objects __c_locale/__c_dot_utf8_locale (src/locale/c_locale.c) — none of
    # which this OS compiles into libc. With them included, libc.so links (the symbols
    # are just UND) but the dynamic loader fails at runtime: "Error relocating
    # /lib/ld-musl-x86_64.so.1: iswspace: symbol not found" (and wcsnlen/iswdigit/
    # __c_locale/__c_dot_utf8_locale). This OS has NO wide-char stdio consumer and does
    # NOT publish <wchar.h> (where the w* decls live), so excluding them loses nothing
    # and keeps the ABI/locale/ctype dependency chain out of libc. If wide-char stdio is
    # ever needed: publish <wchar.h>, add a libc.map <wchar.h> block, and compile
    # src/ctype/isw*.c + src/string/wcsnlen.c + src/locale/c_locale.c + btowc/wctob.
    ${MUSL_DIR}/src/stdio/fgetwc.c
    ${MUSL_DIR}/src/stdio/fgetws.c
    ${MUSL_DIR}/src/stdio/fputwc.c
    ${MUSL_DIR}/src/stdio/fputws.c
    ${MUSL_DIR}/src/stdio/fwide.c
    ${MUSL_DIR}/src/stdio/fwprintf.c
    ${MUSL_DIR}/src/stdio/fwscanf.c
    ${MUSL_DIR}/src/stdio/getwc.c
    ${MUSL_DIR}/src/stdio/getwchar.c
    ${MUSL_DIR}/src/stdio/open_wmemstream.c
    ${MUSL_DIR}/src/stdio/putwc.c
    ${MUSL_DIR}/src/stdio/putwchar.c
    ${MUSL_DIR}/src/stdio/swprintf.c
    ${MUSL_DIR}/src/stdio/swscanf.c
    ${MUSL_DIR}/src/stdio/ungetwc.c
    ${MUSL_DIR}/src/stdio/vfwprintf.c
    ${MUSL_DIR}/src/stdio/vfwscanf.c
    ${MUSL_DIR}/src/stdio/vswprintf.c
    ${MUSL_DIR}/src/stdio/vswscanf.c
    ${MUSL_DIR}/src/stdio/vwprintf.c
    ${MUSL_DIR}/src/stdio/vwscanf.c
    ${MUSL_DIR}/src/stdio/wprintf.c
    ${MUSL_DIR}/src/stdio/wscanf.c)
add_musl_lib(musl_stdio_objs SOURCES ${MUSL_STDIO_SOURCES})

# ===================== musl multibyte subset (narrow stdio %ls/%lc only) ====================
# vfprintf.c references wctomb (%ls/%lc wide-string formatting) and vfscanf.c
# references mbrtowc + mbsinit (%ls scanning) — these are link-time refs in the
# narrow-char printf/scanf path, so they must be satisfied. They live in musl
# src/multibyte, which no other musl_*_objs compiles. We compile only the minimal
# narrow-path closure, NOT the whole src/multibyte glob:
#   wctomb.c  → calls wcrtomb
#   wcrtomb.c → UTF-8 encoder (MB_CUR_MAX resolves inline; no link ref to bittab)
#   mbrtowc.c → UTF-8 decoder, references bittab (the __fsmu8 table)
#   mbsinit.c → pure (no deps)
#   internal.c → provides bittab (__fsmu8)
# The OTHER multibyte files (btowc/wctob/mbtowc/mblen/mbsrtowcs/wcsrtombs/...) are
# NOT compiled: they are only referenced by the wide-char stdio files (vfwprintf/
# fgetwc/...), which are excluded from musl_stdio_objs above. Including them would
# pull the C-locale objects __c_locale/__c_dot_utf8_locale (src/locale/c_locale.c)
# and recreate the runtime reloc failure. MB_CUR_MAX → (CURRENT_UTF8 ? 4 : 1) reads
# __pthread_self()->locale at runtime (set by __init_tp in musl_pthread) — no link
# dep on __c_locale. No public API here is exported (backs stdio internals).
add_musl_lib(musl_multibyte_objs SOURCES
    ${MUSL_DIR}/src/multibyte/wctomb.c
    ${MUSL_DIR}/src/multibyte/wcrtomb.c
    ${MUSL_DIR}/src/multibyte/mbrtowc.c
    ${MUSL_DIR}/src/multibyte/mbsinit.c
    ${MUSL_DIR}/src/multibyte/internal.c)

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
               $<TARGET_OBJECTS:musl_time_objs>
               $<TARGET_OBJECTS:musl_stdio_objs>
               $<TARGET_OBJECTS:musl_multibyte_objs>
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
# loader-only, non-pthread symbols (dprintf/vdprintf/__tlsdesc_*/__libc_get_version)
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
               $<TARGET_OBJECTS:musl_time_objs>
               $<TARGET_OBJECTS:musl_stdio_objs>
               $<TARGET_OBJECTS:musl_multibyte_objs>
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
    # only libm + errno; __shgetc fallback-references __uflow). __uflow is now
    # supplied by musl src/stdio/__uflow.c in the musl_stdio_objs glob (the old
    # scan_uflow_stub.c stub is deleted — see the __stdio_exit/at_quick_exit note).
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
    # __uflow: musl's src/stdio/__uflow.c (musl_stdio_objs glob) now provides the
    # real definition that satisfies the link-time reference musl's shgetc.c
    # emits. The hand-written user/lib/musl_shim/scan_uflow_stub.c stub (added
    # when stdio was NOT yet migrated and __uflow existed nowhere) is deleted —
    # it would multi-define __uflow against the real one.
)
add_musl_lib(musl_stdlib_objs SOURCES ${MUSL_STDLIB_SOURCES})

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
# wcsftime.c EXCLUDED: needs wchar tier (wcstoul/mbstowcs/wmemcpy not yet
# migrated), same as musl_string_objs excludes wcs*/wmem*. Deferred.
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

# ===== libinput (static library, keyboard subset) — see build_script/third_party/libinput/libinput.cmake =====
# Static `input` + shared `libinput.so` build rules extracted to keep this file
# focused on libc + user ELFs. include() runs in this scope, so the targets and
# LIBINPUT_* vars behave exactly as when inlined.
include(${CMAKE_SOURCE_DIR}/build_script/third_party/libinput/libinput.cmake)

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

# ===== libdrm (third_party/drm, core only) — see build_script/third_party/libdrm/libdrm.cmake =====
# Static `drm` + shared `drm_so` build rules extracted. include() runs in this
# scope and BEFORE the drm_test_link ELF below, so `drm`/`drm_so` are defined
# when it links them.
include(${CMAKE_SOURCE_DIR}/build_script/third_party/libdrm/libdrm.cmake)

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

# ===== libffi (third_party/libffi, full-feature incl. closures) — see build_script/third_party/libffi/libffi.cmake =====
# Shared `ffi.so` build rules extracted. include() runs in this scope and before
# wayland (which SO_LINK_LIBS ffi) / libexpat.
include(${CMAKE_SOURCE_DIR}/build_script/third_party/libffi/libffi.cmake)

# ===================== libexpat.so — see build_script/third_party/libexpat/libexpat.cmake =====================
# Shared `expat.so` build rules extracted. Sets LIBEXPAT_DIR (consumed by
# wayland's host scanner) — include() runs in this scope, BEFORE wayland.cmake.
include(${CMAKE_SOURCE_DIR}/build_script/third_party/libexpat/libexpat.cmake)

# ===================== wayland (third_party/wayland, 1.25.91) — see build_script/third_party/wayland/wayland.cmake =====================
# Host wayland-scanner + 3 freestanding .so (server/client/cursor) build rules
# extracted. include() runs in this scope, AFTER libffi.cmake (provides `ffi`)
# and libexpat.cmake (provides LIBEXPAT_DIR for the host scanner).
include(${CMAKE_SOURCE_DIR}/build_script/third_party/wayland/wayland.cmake)
