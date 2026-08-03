# modules/linux.cmake — musl src/linux integration (linux_worklist).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, raw add_library, USER_FREESTANDING_FLAGS.
# ===================== musl src/linux integration =====================
# Build the subset of upstream musl src/linux/*.c whose routed syscalls this
# kernel already implements, into libc via two OBJECT libs (musl_linux_objs
# -fno-pie + musl_linux_objs_so -fPIC), mirroring musl_fcntl_objs / musl_dl_objs.
#
# Selection rule (linux_worklist): a file is INCLUDED only if EVERY syscall its
# musl wrapper routes to is implemented by the kernel (kernel/bsd/syscall.c OR
# kernel/xcore/trap.c — two-layer dispatch, either counts). Multi-path files
# (e.g. pwritev2 tries SYS_pwritev2 then falls back to SYS_pwritev) are EXCLUDED
# if ANY path's syscall is missing — otherwise an ENOSYS symbol leaks into libc.
# Files needing complex kernel machinery (ptrace/xattr/fanotify/quotactl/
# module/klogctl/pivot_root/swap/setns/unshare/reboot/vhangup/process_vm/
# copy_file_range/...) are excluded; they stay unimplemented. inotify is NOW
# implemented (kernel/bsd/inotify.c — init1/add_watch/rm_watch), so musl's
# src/linux/inotify.c is included below. Files already compiled by another musl
# module are excluded to avoid multi-define.
#
# NOT globbed on purpose: ~52 of the 67 src/linux files route to unimplemented
# syscalls or drag heavy deps — a glob + giant REMOVE_ITEM is no clearer than an
# explicit include list, and an explicit list documents exactly what's supported.
#
# Already-compiled elsewhere (multi-define guard, NOT listed here):
#   getdents.c    — dirent module
#   membarrier.c  — pthread module
#   prlimit.c     — resource module
#   sendfile.c    — unistd module
#
# INCLUDED (15) — each routes only to kernel-implemented syscalls (or reads TLS,
# no syscall):
#   arch_prctl.c   SYS_arch_prctl (bsd)            [new symbol, no repo wrapper]
#   brk.c          returns -ENOMEM (no syscall)    [new symbol; kernel sys_brk is a fail stub]
#   epoll.c        SYS_epoll_{create,create1,ctl,wait,pwait} (bsd)
#   eventfd.c      SYS_eventfd2 (bsd); SYS_eventfd fallback unreachable [replaces io_multiplex.cc]
#   fallocate.c    SYS_fallocate (bsd, lowercase)  [new symbol]
#   flock.c        SYS_flock (bsd)                 [replaces sys_socket.cc]
#   getrandom.c    SYS_getrandom (bsd, cancellable)[replaces getrandom.c; KEEP arc4random_* — musl has none; getentropy now from misc.cmake]
#   gettid.c       reads __pthread_self()->tid (no syscall; tid set in __init_tls via SYS_set_tid_address)
#                                                                  [replaces sys_process.cc]
#   ioperm.c       SYS_ioperm (xcore/trap.c)       [replaces unistd.cc; umask now from unistd.cmake src/stat/umask.c — unistd.cc deleted]
#   inotify.c      SYS_inotify_init1/add_watch/rm_watch (bsd) [new symbols; inotify_init musl fallback is dead — init1 implemented]
#   memfd_create.c SYS_memfd_create (bsd)          [replaces sys_process.cc; mman module does NOT compile it]
#   prctl.c        SYS_prctl (bsd)                 [replaces musl_missing.c]
#   sbrk.c         SYS_brk (bsd, fail stub)        [new symbol]
#   sethostname.c  SYS_sethostname (bsd)           [replaces uname.c; gethostname STAYS — musl's returns
#                                                   uname.nodename="(none)", repo reads live sys_gethostname]
#   signalfd.c     SYS_signalfd4 (bsd); kernel accepts sizemask>=8 (musl passes _NSIG/8=8, repo passed
#                   sizeof(sigset_t)=128 — both OK, kernel reads low 8 bytes)  [replaces io_multiplex.cc]
#   statx.c        SYS_statx (bsd); fstatat fallback unreachable (SYS_statx implemented).
#                   Core stat wrappers now come from modules/stat.cmake.
#
# Multi-path/deps EXCLUDED (keep repo hand-written where one exists):
#   mount.c        umount/umount2 → SYS_umount2 missing; repo sys_ipc.cc mount stays
#   timerfd.c      timerfd_gettime → SYS_timerfd_gettime missing; repo timerfd_create/settime stay
#   utimes.c       deps __futimesat (src/stat/futimesat.c, compiled nowhere) → link fail
#   renameat2.c    flags≠0 → SYS_renameat2 missing
#   preadv2.c      flags≠0 → SYS_preadv2 missing
#   pwritev2.c     both SYS_pwritev (flags==0) and SYS_pwritev2 (flags≠0) missing
#   wait4.c        time64 fallback → SYS_wait4_time64 (dead branch, but strict rule)
#   wait3.c        calls wait4 (excluded)
# Candidates needing only a simple kernel syscall (tracked separately, not this batch):
#   renameat2 (SYS_renameat2~SYS_RENAMEAT), pwritev/preadv2/pwritev2, timerfd_gettime,
#   chroot, personality, syncfs, mlock2, umount2, iopl, settimeofday/stime,
#   setfsgid/setfsuid, setgroups, etc.
# sysinfo.c — NOW ADOPTED below: SYS_sysinfo is implemented in the kernel
#   (fills struct sysinfo totalram/freeram); musl __lsysinfo/sysinfo supply
#   the public symbols and back sysconf(_SC_PHYS_PAGES/_SC_AVPHYS_PAGES).
#
# gettid.c includes "pthread_impl.h" (musl-internal) → needs src/internal on the
# include path (provided below, same as dl.cmake). It reads __pthread_self()->tid,
# which __init_tls.c sets via SYS_set_tid_address — verified initialized.
set(MUSL_LINUX_SOURCES
    ${MUSL_DIR}/src/linux/arch_prctl.c
    ${MUSL_DIR}/src/linux/brk.c
    ${MUSL_DIR}/src/linux/epoll.c
    ${MUSL_DIR}/src/linux/eventfd.c
    ${MUSL_DIR}/src/linux/fallocate.c
    ${MUSL_DIR}/src/linux/flock.c
    ${MUSL_DIR}/src/linux/getrandom.c
    ${MUSL_DIR}/src/linux/gettid.c
    ${MUSL_DIR}/src/linux/ioperm.c
    ${MUSL_DIR}/src/linux/inotify.c
    ${MUSL_DIR}/src/linux/memfd_create.c
    ${MUSL_DIR}/src/linux/prctl.c
    ${MUSL_DIR}/src/linux/sbrk.c
    ${MUSL_DIR}/src/linux/sethostname.c
    ${MUSL_DIR}/src/linux/signalfd.c
    ${MUSL_DIR}/src/linux/statx.c
    ${MUSL_DIR}/src/linux/sysinfo.c)

add_library(musl_linux_objs OBJECT ${MUSL_LINUX_SOURCES})
target_include_directories(musl_linux_objs PRIVATE
    ${MUSL_GEN_INCLUDE_DIR}
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_linux_objs PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fno-pie -Wno-all)

# libc.so PIC mirror (same as musl_fcntl_objs_so / musl_dl_objs_so).
add_library(musl_linux_objs_so OBJECT ${MUSL_LINUX_SOURCES})
target_include_directories(musl_linux_objs_so PRIVATE
    ${MUSL_GEN_INCLUDE_DIR}
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_linux_objs_so PRIVATE
    -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fPIC -Wno-all)
add_dependencies(musl_linux_objs musl_headers)
add_dependencies(musl_linux_objs_so musl_headers)
