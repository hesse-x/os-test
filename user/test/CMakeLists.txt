# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/test/ — Unity-based test ELF targets

# Test runner (no Unity needed). Propagate -DLIBCXX when libc++ is enabled so the
# static tests[] table's libcxx_smoke entry (guarded by #if defined(LIBCXX) in
# test_runner.c) is compiled in. See refact_cmake.md §3.6.
set(_TEST_RUNNER_DEFS "")
if(TEST)
    list(APPEND _TEST_RUNNER_DEFS TEST)
endif()
if(LIBCXX)
    list(APPEND _TEST_RUNNER_DEFS LIBCXX)
endif()
if(_TEST_RUNNER_DEFS)
    add_user_dyn_elf(test_runner C SOURCES test_runner.c LINK_LIBS c DEFS ${_TEST_RUNNER_DEFS})
else()
    add_user_dyn_elf(test_runner C SOURCES test_runner.c LINK_LIBS c)
endif()

# EGL/GLES2 end-to-end smoke. Mesa is built and staged by build.sh after the
# main CMake ninja, so this target deliberately stays out of ALL and is built
# on demand once build/libEGL.so and build/libGLESv2.so exist.
if(TEST)
    set(EGL_SMOKE_ELF ${CMAKE_BINARY_DIR}/test_egl_smoke.elf)
    set(EGL_SMOKE_OBJ ${CMAKE_BINARY_DIR}/test_egl_smoke.elf.0.o)
    add_custom_command(OUTPUT ${EGL_SMOKE_OBJ}
        COMMAND ${CMAKE_C_COMPILER} ${USER_COMPILE_FLAGS} ${USER_BUILD_FLAGS}
                -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/third_party
                -I${CMAKE_SOURCE_DIR}/include/uapi -I${CMAKE_SOURCE_DIR}/user/include
                ${MUSL_INCLUDE_FLAGS} ${DRM_INCLUDE_FLAGS}
                -I${CMAKE_SOURCE_DIR}/third_party/mesa/include
                -c ${CMAKE_CURRENT_SOURCE_DIR}/test_egl_smoke.c -o ${EGL_SMOKE_OBJ}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_egl_smoke.c ${MUSL_GEN_HEADERS}
        IMPLICIT_DEPENDS C ${CMAKE_CURRENT_SOURCE_DIR}/test_egl_smoke.c
        COMMENT "Compiling test_egl_smoke.elf.0.o")
    add_custom_command(OUTPUT ${EGL_SMOKE_ELF}
        COMMAND ${CMAKE_C_COMPILER} -fno-pie -no-pie
                -Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1
                -Wl,--hash-style=gnu -Wl,--no-as-needed -Wl,--allow-shlib-undefined
                -nostdlib -nodefaultlibs -o ${EGL_SMOKE_ELF}
                ${MUSL_LIB_DIR}/Scrt1.o ${MUSL_LIB_DIR}/crti.o ${EGL_SMOKE_OBJ}
                ${CMAKE_BINARY_DIR}/libEGL.so ${CMAKE_BINARY_DIR}/libGLESv2.so
                ${CMAKE_BINARY_DIR}/libc.so ${MUSL_LIB_DIR}/crtn.o
        DEPENDS ${EGL_SMOKE_OBJ} ${MUSL_LIB_DIR}/Scrt1.o ${MUSL_LIB_DIR}/crti.o
                ${MUSL_LIB_DIR}/crtn.o ${CMAKE_BINARY_DIR}/libEGL.so
                ${CMAKE_BINARY_DIR}/libGLESv2.so ${CMAKE_BINARY_DIR}/libc.so
        COMMENT "Linking test_egl_smoke.elf")
    add_custom_target(test_egl_smoke_elf DEPENDS ${EGL_SMOKE_ELF})
    add_dependencies(test_egl_smoke_elf musl_libc c_so)
    os_image_path(test_egl_smoke_elf test_egl_smoke.elf test/test_egl_smoke.elf PARTITION 2)
endif()

# Layer 1: unistd (pipe/read/write/close)
add_user_dyn_elf(pipe C SOURCES test_pipe.c LINK_LIBS c STATIC_LIBS unity)

# Layer 2: fcntl (open/close/dup2/fcntl/lseek)
add_user_dyn_elf(fcntl C SOURCES test_fcntl.c LINK_LIBS c STATIC_LIBS unity)

# 05: OFD (F_OFD_*) record locks + F_SETOWN_EX/F_GETOWN_EX.
add_user_dyn_elf(fcntl_ofd C SOURCES test_fcntl_ofd.c LINK_LIBS c STATIC_LIBS unity)

# BSD flock(2) + accept4(2) (SOCK_CLOEXEC / SOCK_NONBLOCK).
add_user_dyn_elf(flock C SOURCES test_flock.c LINK_LIBS c STATIC_LIBS unity)
add_user_dyn_elf(accept4 C SOURCES test_accept4.c LINK_LIBS c STATIC_LIBS unity)

# Layer 3: string / malloc / stdio
add_user_dyn_elf(string C SOURCES test_string.c LINK_LIBS c STATIC_LIBS unity)
add_user_dyn_elf(malloc C SOURCES test_malloc.c LINK_LIBS c STATIC_LIBS unity)
add_user_dyn_elf(stdio C SOURCES test_stdio.c LINK_LIBS c STATIC_LIBS unity)

# Layer 4: sys/mman
add_user_dyn_elf(mmap C SOURCES test_mmap.c LINK_LIBS c STATIC_LIBS unity DEFS _GNU_SOURCE)

# Layer 5: IPC
add_user_dyn_elf(ipc C SOURCES test_ipc.c LINK_LIBS c STATIC_LIBS unity)

# Layer 6: socket
add_user_dyn_elf(socket C SOURCES test_socket.c LINK_LIBS c STATIC_LIBS unity)

# S16: recvmsg/sendmsg/poll flag semantics — MSG_PEEK / MSG_TRUNC / MSG_WAITALL
# / EPIPE→SIGPIPE (MSG_NOSIGNAL) / SCM_RIGHTS MSG_CTRUNC / poll POLLNVAL /
# msg_flags writeback.
add_user_dyn_elf(test_socket_msgflags C SOURCES test_socket_msgflags.c LINK_LIBS c STATIC_LIBS unity)

# S17: AF_UNIX SOCK_DGRAM + recvmsg msg_name — bind/sendto path resolution,
# message boundaries, connect default target, MSG_PEEK/MSG_TRUNC, SCM_RIGHTS,
# EPROTOTYPE/ENOENT error semantics. TEST-gated (bind paths live under /run).
if(TEST)
    add_user_dyn_elf(test_unix_dgram C SOURCES test_unix_dgram.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
endif()

# Layer 7: process
add_user_dyn_elf(process C SOURCES test_process.c LINK_LIBS c STATIC_LIBS unity)
add_user_dyn_elf(test_spawn_popen C SOURCES test_spawn_popen.c LINK_LIBS c STATIC_LIBS unity DEFS _GNU_SOURCE)

# Layer 8: signal
add_user_dyn_elf(signal C SOURCES test_signal.c LINK_LIBS c STATIC_LIBS unity)

# S01: job-control stop state (SIGSTOP/SIGTSTP/SIGCONT + WUNTRACED reporting)
add_user_dyn_elf(signal_stop C SOURCES test_signal_stop.c LINK_LIBS c STATIC_LIBS unity)
# S02: WCOREDUMP + SA_RESETHAND + SA_NODEFER + sa_restorer user trampoline
add_user_dyn_elf(signal_flags C SOURCES test_signal_flags.c LINK_LIBS c STATIC_LIBS unity)
# S03: kill/tgkill permission & scope + set_tid_address 64-bit + per-process alarm
add_user_dyn_elf(kill_perm C SOURCES test_kill_perm.c LINK_LIBS c STATIC_LIBS unity)
# S04: sigaltstack + SA_ONSTACK alternate-signal-stack delivery
add_user_dyn_elf(sigaltstack C SOURCES test_sigaltstack.c LINK_LIBS c STATIC_LIBS unity)

# Layer 9: poll
add_user_dyn_elf(poll C SOURCES test_poll.c LINK_LIBS c STATIC_LIBS unity)

# Layer 10: PCI/device
add_user_dyn_elf(pci C SOURCES test_pci.c LINK_LIBS c STATIC_LIBS unity)

# Layer 11: ioctl/fstat/isatty
add_user_dyn_elf(ioctl C SOURCES test_ioctl.c LINK_LIBS c STATIC_LIBS unity)

# Layer 12: device VFS unified path
add_user_dyn_elf(dev_vfs C SOURCES test_dev_vfs.c LINK_LIBS c STATIC_LIBS unity)

# FPU/SSE smoke test: explicit inline-asm movsd+mulsd to verify user-mode
# SSE path (CR4.OSFXSR + lazy FPU #NM handler). No Unity — simple printf+exit.
add_user_dyn_elf(test_sse_smoke C SOURCES test_sse_smoke.c LINK_LIBS c)

# CPU-bound stress test for work-stealing balance verification (launched
# manually as multiple instances; not a test_runner regression)
add_user_dyn_elf(test_cpu_burn C SOURCES test_cpu_burn.c LINK_LIBS c NO_IMAGE)

# Layer 13: pthread (musl upstream — musl_pthread is merged into libc.a and
# libc.so via MERGE_LIBS, so pthread_* / __init_tls / __errno_location come
# from libc alone; no separate musl_pthread link needed).
add_user_dyn_elf(pthread C SOURCES test_pthread.c LINK_LIBS c STATIC_LIBS unity)

# Layer 13b: C11 <threads.h> (musl src/thread/{thrd,mtx,cnd,tss,call_once}_*.c,
# same musl_pthread glob; symbols exported via libc.map <threads.h> block).
add_user_dyn_elf(c11threads C SOURCES test_c11threads.c LINK_LIBS c STATIC_LIBS unity)

# musl src/sched glob: verify the GNU sched_getcpu wrapper reaches getcpu(2).
add_user_dyn_elf(test_sched C SOURCES test_sched.c LINK_LIBS c STATIC_LIBS unity DEFS _GNU_SOURCE)

# DRM virtio-gpu test (no Unity — diagnostic printf)
add_user_dyn_elf(drm_test C SOURCES drm_test.c LINK_LIBS c)

# Layer 14: DRM ioctl (GET_MAGIC, AUTH_MAGIC, ADDFB2, GEM_CLOSE, GETFB, caps)
add_user_dyn_elf(drm_ioctl C SOURCES test_drm_ioctl.c LINK_LIBS c STATIC_LIBS unity)

# Phase C: DRM property/master/cursor/cleanup tests
add_user_dyn_elf(drm_phase_c C SOURCES test_drm_mode.c LINK_LIBS c STATIC_LIBS unity)

# virgl (GL) legacy command plane (GETPARAM/GET_CAPS/CONTEXT_INIT/
# RESOURCE_CREATE v1/TRANSFER/WAIT/GEM_CLOSE). Raw ioctls, no libdrm.
# Host-independent capability probes assert; host-dependent virgl-path cases
# self-skip when no virglrenderer back-end (capset 1/2) is present.
add_user_dyn_elf(virgl_channel C SOURCES test_virgl_channel.c LINK_LIBS c STATIC_LIBS unity)

# Layer 15: ioctl variable-length (user-space driver proxy, RECV_IOCTL path)
add_user_dyn_elf(ioctl_varlen C SOURCES test_ioctl_varlen.c LINK_LIBS c STATIC_LIBS unity)

# Layer 16: epoll
add_user_dyn_elf(epoll C SOURCES test_epoll.c LINK_LIBS c STATIC_LIBS unity)

# S18: EPOLLONESHOT (report-once + disarm + MOD re-arm) / EPOLLEXCLUSIVE
# (anti-thundering-herd unicast wake) / epoll_ctl epfd==fd self-register EINVAL
# / EXCLUSIVE MOD-toggle EINVAL.
add_user_dyn_elf(test_epoll_oneshot C SOURCES test_epoll_oneshot.c LINK_LIBS c STATIC_LIBS unity)
# S18: accept() no longer arms a 30s false timeout — idle blocking accept blocks
# past 30s until SIGALRM/EINTR. NOTE: sleeps up to 35s by design (regression proof).
add_user_dyn_elf(test_accept_no_timeout C SOURCES test_accept_no_timeout.c LINK_LIBS c STATIC_LIBS unity)

# Layer 17: eventfd
add_user_dyn_elf(eventfd C SOURCES test_eventfd.c LINK_LIBS c STATIC_LIBS unity)

# Layer 18: timerfd
add_user_dyn_elf(timerfd C SOURCES test_timerfd.c LINK_LIBS c STATIC_LIBS unity)

# Layer 19: signalfd
add_user_dyn_elf(signalfd C SOURCES test_signalfd.c LINK_LIBS c STATIC_LIBS unity)

# S14: CLOCK_REALTIME wall clock (CMOS RTC) + clockid set + clock_settime
add_user_dyn_elf(test_clock_realtime C SOURCES test_clock_realtime.c LINK_LIBS c STATIC_LIBS unity)
# S15: nanosleep/clock_nanosleep (relative + TIMER_ABSTIME) + EINTR rem
add_user_dyn_elf(test_time_sleep C SOURCES test_time_sleep.c LINK_LIBS c STATIC_LIBS unity)
# S15: CLOCK_PROCESS_CPUTIME_ID process-level CPU time
add_user_dyn_elf(test_clock_cputime C SOURCES test_clock_cputime.c LINK_LIBS c STATIC_LIBS unity)
# membarrier(2): QUERY bitmask, advertised-command success, -EINVAL on unknown
# cmd/flags, peer-thread ordering smoke (x86-64 TSO: local fence, no IPI).
add_user_dyn_elf(test_membarrier C SOURCES test_membarrier.c LINK_LIBS c STATIC_LIBS unity)

# <inttypes.h> → musl: strtoimax/strtoumax/imaxabs/imaxdiv/wcstoimax (musl
# stdlib/wchar objs) + PRI*/SCN* format macros (musl header, "l" modifier
# matching int64_t=long; repo macro-only shim deleted). Pure compute, no FS.
add_user_dyn_elf(test_inttypes C SOURCES test_inttypes.c LINK_LIBS c STATIC_LIBS unity)

# musl temp/syslog APIs plus GNU startup-name variables. syslogd is not part of
# this image, so the test covers the libc API state rather than log delivery.
add_user_dyn_elf(test_musl_misc C SOURCES test_musl_misc.c LINK_LIBS c STATIC_LIBS unity DEFS _GNU_SOURCE)

# musl src/misc glob migration (modules/misc.cmake): getopt/getopt_long,
# a64l/l64a, getsubopt, get_current_dir_name, gethostid, getauxval,
# ptsname/ptsname_r, mntent, lockf, nftw, fmtmsg, uname(6-field ABI fix).
add_user_dyn_elf(test_misc C SOURCES test_misc.c LINK_LIBS c STATIC_LIBS unity DEFS _GNU_SOURCE)

# <locale.h> → musl: 6 POSIX locale managers (setlocale/localeconv/newlocale/
# duplocale/freelocale/uselocale) + 4 collate (strcoll/strxfrm/wcscoll/wcsxfrm
# +_l), built by musl_locale_objs (src/locale/*). C-locale only (no .mo catalog
# / no iconv in this OS) — collate are musl's code-point stubs (strcmp/strlen/
# wcscmp/wcslen). Pure compute + stdio/wchar, no FS. No DEFS: test default
# _XOPEN_SOURCE 700 (musl features.h) already gates the locale_t API.
add_user_dyn_elf(test_locale C SOURCES test_locale.c LINK_LIBS c STATIC_LIBS unity)

# POSIX regular expressions + shell-pattern matching from musl src/regex.
add_user_dyn_elf(test_regex C SOURCES test_regex.c LINK_LIBS c STATIC_LIBS unity)

# Layer 20: getrandom (/dev/(u)random + libc wrapper)
add_user_dyn_elf(getrandom C SOURCES test_getrandom.c LINK_LIBS c STATIC_LIBS unity)

# mprotect + PROT_NONE round-trip + sysconf (plan1.md §1.5 gap 1 hard acceptance)
add_user_dyn_elf(test_mprotect C SOURCES test_mprotect.c LINK_LIBS c STATIC_LIBS unity)

# mount framework test
add_user_dyn_elf(mount C SOURCES test_mount.c LINK_LIBS c STATIC_LIBS unity)

# sysfs + file_operations + ringbuf (TEST build only — evdev sysfs needs stubs)
if(TEST)
    add_user_dyn_elf(test_sysfs C SOURCES test_sysfs.c LINK_LIBS c STATIC_LIBS unity DEFS TEST INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/libinput/include/linux)
    add_user_dyn_elf(test_procfs C SOURCES test_procfs.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
endif()

# Phase B: libdrm.so composite loading test (plan_drm2)
# LINK_LIBS drm c → add_user_dyn_elf internally auto-add_dependencies(drm_so/c_so).
# NO_IMAGE: manual diagnostic ELF, not in test_runner's table.
add_user_dyn_elf(hello_drm_dyn C SOURCES hello_drm_dyn.c LINK_LIBS drm c NO_IMAGE)

# Phase B: modetest — drmOpen + GETRESOURCES + CONNECTOR + DUMB + ADDFB + SETCRTC + PAGE_FLIP
add_user_elf(modetest C SOURCES modetest.c LINK_LIBS drm c)
add_dependencies(modetest_elf libdrm_fourcc_table)

# LINK_LIBS drm c → add_user_dyn_elf internally auto-add_dependencies(drm_so).
# NO_IMAGE: manual diagnostic ELF, not in test_runner's table.
add_user_dyn_elf(modetest_dyn C SOURCES modetest_dyn.c LINK_LIBS drm c NO_IMAGE)

# libudev shim phase A regression (TEST build — depends on real evdev registration).
# Mirrors test_sysfs: TEST-gated + udev-shim header include path.
# SOURCES compiles udev.c directly: udev_* symbols are implemented in that file,
# standalone compilation suffices, no need to link the whole input static lib
# (avoids pulling in all libinput deps). Default -Iinclude/uapi already covers
# udev.c's <xos/input.h>/<xos/ioctl.h>.
if(TEST)
    add_user_dyn_elf(test_libudev C SOURCES test_libudev.c ${CMAKE_SOURCE_DIR}/user/lib/udev-shim/udev.c LINK_LIBS c STATIC_LIBS unity DEFS TEST INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/user/lib/udev-shim)
endif()

# udevd daemon channel end-to-end test (TEST build — depends on real udevd
# runtime + evdev registration).
# Mirrors test_libudev: TEST-gated + udev-shim header include path.
# test_udevd calls udev_* symbols directly → SOURCES compiles udev.c (avoids
# pulling in the whole input static lib).
if(TEST)
    add_user_dyn_elf(test_udevd C SOURCES test_udevd.c ${CMAKE_SOURCE_DIR}/user/lib/udev-shim/udev.c LINK_LIBS c STATIC_LIBS unity DEFS TEST INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/user/lib/udev-shim)
endif()

# udevd db + rules engine end-to-end test (TEST build — depends on real udevd
# runtime + evdev registration).
# Mirrors test_libudev: TEST-gated + udev-shim header include path.
# test_udevd_db calls udev_* symbols directly → SOURCES compiles udev.c.
if(TEST)
    add_user_dyn_elf(test_rename C SOURCES test_rename.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_udevd_db C SOURCES test_udevd_db.c ${CMAKE_SOURCE_DIR}/user/lib/udev-shim/udev.c LINK_LIBS c STATIC_LIBS unity DEFS TEST INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/user/lib/udev-shim)
endif()

# devtmpfs device-table dynamic regression (work2_design §5.5.1) — TEST-gated,
# does not call udev_*, only sys_dev_create/open/opendir (via libc + syscall),
# no need to compile in udev.c.
if(TEST)
    add_user_dyn_elf(test_dev_vfs_dynamic C SOURCES test_dev_vfs_dynamic.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
endif()

# VFS i_op dispatch + inode refcount tests (TEST build only)
if(TEST)
    add_user_dyn_elf(test_vfs_dispatch C SOURCES test_vfs_dispatch.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_inode_refcount C SOURCES test_inode_refcount.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_tmpfs_socket C SOURCES test_tmpfs_socket.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_dirent_seek C SOURCES test_dirent_seek.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_dirent_complete C SOURCES test_dirent_complete.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_resource C SOURCES test_resource.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_cloexec_perfd C SOURCES test_cloexec_perfd.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_pipe2 C SOURCES test_pipe2.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_openat_dirfd C SOURCES test_openat_dirfd.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_stat_real C SOURCES test_stat_real.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_statx C SOURCES test_statx.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # path-based inode metadata/link syscalls (§3.2/§3.5): access/faccessat by
    # euid (Q4); utimensat in-memory timestamps (Q5). Mirrors test_stat_real
    # style, FAT32+tmpfs dual fixture. Phase 3 link part enabled via TEST_LINK
    # (see test_link_utimensat.c).
    add_user_dyn_elf(test_access C SOURCES test_access.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_link_utimensat C SOURCES test_link_utimensat.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # §3.3 symlink/readlink (tmpfs real impl, Q2): round-trip / lstat vs stat /
    # EEXIST / FAT32 ENOSYS / readlink non-softlink EINVAL. Mirrors test_stat_real.
    add_user_dyn_elf(test_symlink C SOURCES test_symlink.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # §3.4 link/linkat (tmpfs hardlink + full nlink path, Q3): data sharing /
    # unlink keeps data nlink-- / directory EPERM / cross-fs EXDEV / FAT32 EPERM
    # / EEXIST / flags EINVAL.
    add_user_dyn_elf(test_link C SOURCES test_link.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # chmod/chown family (phase 1 capable closure): mode/uid/gid truly write
    # inode + setuid-bit clear rules (apply_chmod/apply_chown: root CAP_FSETID
    # retained / non-privileged cleared). Persistence is in-memory only (like
    # utimensat). Non-root cases via fork child setuid(1000) + _exit code +
    # parent waitpid.
    add_user_dyn_elf(test_chmod C SOURCES test_chmod.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_vma_restructure C SOURCES test_vma_restructure.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_mmap_addr_hint C SOURCES test_mmap_addr_hint.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # S12: MAP_PRIVATE+fd file-backed mmap + demand-fault page-in + COW.
    add_user_dyn_elf(test_mmap_file_private C SOURCES test_mmap_file_private.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # S13: munmap partial/hole-punch + mprotect interval + flag handling + size limit.
    add_user_dyn_elf(test_munmap_partial C SOURCES test_munmap_partial.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_mprotect_partial C SOURCES test_mprotect_partial.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_mmap_flags C SOURCES test_mmap_flags.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_mmap_size_limit C SOURCES test_mmap_size_limit.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # S19: process cluster — clone exit_signal/SETTID-fault, wait4 pgid/rusage,
    # setuid saved-set ladder, execve VFS read.
    add_user_dyn_elf(test_clone_exit_signal C SOURCES test_clone_exit_signal.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_clone_settid_fault C SOURCES test_clone_settid_fault.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_wait4_pgid_rusage C SOURCES test_wait4_pgid_rusage.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_wait4_options C SOURCES test_wait4_options.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_setuid_saved C SOURCES test_setuid_saved.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # execve S_ISUID/S_ISGID credential switch verification: setuid_helper.elf
    # reports geteuid/getegid/getuid via its exit code; the main test chmods
    # 04755+chown(1000,1000), forks a child that setuid(2000) drops root, then
    # execve's helper, asserting euid switches to 1000 and real uid is retained.
    # Also exercises setuid-ladder gate self-consistency (phase 0 capable
    # (CAP_SETUID)) regression.
    add_user_dyn_elf(setuid_helper C SOURCES setuid_helper.c LINK_LIBS c)
    add_user_dyn_elf(test_setuid_exec C SOURCES test_setuid_exec.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # M1.1 (unistd.md §2.A1/A2): setresuid/setresgid/setreuid/setregid +
    # getgroups. Permission ladder + -1 unchanged + suid-preservation rule
    # (man setresuid(2)/setreuid(2)), process-wide semantics. Non-root cases via
    # fork child setuid/setresuid(1000) + _exit code + parent waitpid.
    add_user_dyn_elf(test_setxid C SOURCES test_setxid.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_execve_vfs C SOURCES test_execve_vfs.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # musl process module exec-family varargs wrappers (execl/execv/execlp):
    # exercises the varargs→argv conversion that was missing from libc (the
    # `undefined reference to execl` that broke the --cxx libc++ probe link).
    add_user_dyn_elf(test_exec_varargs C SOURCES test_exec_varargs.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # 02: getdents64 chunked resume (tmpfs/sysfs) + SA_RESTART restart +
    # SA_NOCLDWAIT auto-reap.
    add_user_dyn_elf(test_getdents_resume C SOURCES test_getdents_resume.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_sa_restart C SOURCES test_sa_restart.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_sa_nocldwait C SOURCES test_sa_nocldwait.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
endif()

# libffi dynamic test (ffi_worklist §4.C): ffi_call int/double + ffi_closure
# callback (FFI_MMAP_EXEC_WRIT double-map closure path) + mprotect independent
# case.
# LINK_LIBS ffi → -lffi resolves to libffi.so (dynamic link, DT_NEEDED).
# STATIC_LIBS unity → libunity.a (compile-time link; UNITY_EXCLUDE_* propagate
# from the unity target's PUBLIC compile_definitions).
# add_dependencies on target ffi_so ensures build ordering; INCLUDE_DIRS
# ${CMAKE_BINARY_DIR} alone covers all three public headers — ffi.h and
# fficonfig.h (generated) plus ffitarget.h (COPYONLY'd into build/ alongside
# them, matching upstream nodist_include_HEADERS). No reach into src/x86.
# DEFS adds UNITY_INCLUDE_DOUBLE (on top of the shared UNITY_EXCLUDE_*): case 2
# asserts via TEST_ASSERT_EQUAL_DOUBLE, which unity_internals.h:286 gates off
# unless UNITY_INCLUDE_DOUBLE is defined at the test's compile time. Unity is
# linked as libunity.a (UNITY_EXCLUDE_MATH_H propagates
# from the unity target's PUBLIC compile_definitions to this test via
# _collect_iface_compile_definitions), but UNITY_INCLUDE_DOUBLE is test-local
# (only test_ffi needs double asserts), so it stays on this ELF's DEFS. Unity's
# double compare (unity.c:898) is pure arithmetic, no math.h, so it coexists
# with EXCLUDE_MATH.
# GEN_HEADERS lists the generated libffi public headers (configure_file outputs
# in build/) that test_ffi.c #includes via <ffi.h>; bare-gcc custom commands
# don't auto-track these, so editing the template wouldn't re-compile the test.
add_user_dyn_elf(test_ffi C SOURCES test_ffi.c
    LINK_LIBS ffi c
    STATIC_LIBS unity
    INCLUDE_DIRS ${CMAKE_BINARY_DIR}
    GEN_HEADERS ${CMAKE_BINARY_DIR}/ffi.h
                ${CMAKE_BINARY_DIR}/fficonfig.h
                ${CMAKE_BINARY_DIR}/ffitarget.h)
# LINK_LIBS ffi c → add_user_dyn_elf internally auto-add_dependencies(ffi_so/c_so).
# GEN_HEADERS is tracked by each object's custom-command DEPENDS (editing a
# template triggers a rebuild).

# expat smoke test: XML_ParserCreate / Parse / Free / error handling
add_user_dyn_elf(test_expat C SOURCES test_expat.c
    LINK_LIBS expat c
    STATIC_LIBS unity
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/libexpat/expat/lib
                ${CMAKE_BINARY_DIR})

# libwayland-client.so pure-logic test: wl_array (init/add/for_each/copy/release)
# + wl_list (init/insert/remove/length/empty/insert_list). Covers only the
# container layer (no real compositor needed); wl_display_*/wl_proxy paths are
# e2e, out of scope here.
# LINK_LIBS wayland-client → -lwayland-client resolves to libwayland-client.so
# (DT_NEEDED). INCLUDE_DIRS mirrors the wayland_client_so target header paths:
# wayland-client.h (src) → wayland-client-protocol.h (build, generated).
add_user_dyn_elf(test_wayland_client C SOURCES test_wayland_client.c
    LINK_LIBS wayland-client c
    STATIC_LIBS unity
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/wayland/src
                ${CMAKE_SOURCE_DIR}/third_party/wayland
                ${CMAKE_BINARY_DIR}
    GEN_HEADERS ${CMAKE_BINARY_DIR}/wayland-client-protocol.h
                ${CMAKE_BINARY_DIR}/wayland-client-protocol-core.h)

# dlfcn API smoke test (DL-001~006): dlopen(NULL)/dlsym/dlclose/dlerror/dladdr
# against the fused musl loader + musl_dl_objs wrappers. NOT a real .so load
# (that is Phase 3); dlopen(NULL) returns the head handle and dlsym resolves
# against the existing global symbol chain. _GNU_SOURCE for Dl_info/dladdr in
# <dlfcn.h>. No Unity — each step prints a marker and the ELF exits nonzero on
# any failure so test_runner reports [FAIL].
add_user_dyn_elf(test_dl C SOURCES test_dl.c LINK_LIBS c DEFS _GNU_SOURCE)

# /dev/console device test. console aliases the serial port (same serial_ops,
# second devtmpfs node). Guards open/write/poll/stat + read-unsupported, and
# that the node exists for the musl LOG_CONS fallback (syslog.c opens it when
# /dev/log is unreachable).
add_user_dyn_elf(test_console C SOURCES test_console.c LINK_LIBS c STATIC_LIBS unity)
