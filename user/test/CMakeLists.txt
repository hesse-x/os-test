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

# <locale.h> → musl: 6 POSIX locale managers (setlocale/localeconv/newlocale/
# duplocale/freelocale/uselocale) + 4 collate (strcoll/strxfrm/wcscoll/wcsxfrm
# +_l), built by musl_locale_objs (src/locale/*). C-locale only (no .mo catalog
# / no iconv in this OS) — collate are musl's code-point stubs (strcmp/strlen/
# wcscmp/wcslen). Pure compute + stdio/wchar, no FS. No DEFS: test default
# _XOPEN_SOURCE 700 (musl features.h) already gates the locale_t API.
add_user_dyn_elf(test_locale C SOURCES test_locale.c LINK_LIBS c STATIC_LIBS unity)

# POSIX regular expressions + shell-pattern matching from musl src/regex.
add_user_dyn_elf(test_regex C SOURCES test_regex.c LINK_LIBS c STATIC_LIBS unity)

# Layer 20: getrandom (/dev/(u)random + libc 封装)
add_user_dyn_elf(getrandom C SOURCES test_getrandom.c LINK_LIBS c STATIC_LIBS unity)

# mprotect + PROT_NONE round-trip + sysconf (plan1.md §1.5 缺口1 hard acceptance)
add_user_dyn_elf(test_mprotect C SOURCES test_mprotect.c LINK_LIBS c STATIC_LIBS unity)

# mount framework test
add_user_dyn_elf(mount C SOURCES test_mount.c LINK_LIBS c STATIC_LIBS unity)

# sysfs + file_operations + ringbuf (TEST build only — evdev sysfs needs stubs)
if(TEST)
    add_user_dyn_elf(test_sysfs C SOURCES test_sysfs.c LINK_LIBS c STATIC_LIBS unity DEFS TEST INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/libinput/include/linux)
    add_user_dyn_elf(test_procfs C SOURCES test_procfs.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
endif()

# Phase B: libdrm.so composite loading test (plan_drm2)
# LINK_LIBS drm c → add_user_dyn_elf 内部自动 add_dependencies(drm_so/c_so)。
# NO_IMAGE: manual diagnostic ELF, not in test_runner's table.
add_user_dyn_elf(hello_drm_dyn C SOURCES hello_drm_dyn.c LINK_LIBS drm c NO_IMAGE)

# Phase B: modetest — drmOpen + GETRESOURCES + CONNECTOR + DUMB + ADDFB + SETCRTC + PAGE_FLIP
add_user_elf(modetest C SOURCES modetest.c LINK_LIBS drm c)
add_dependencies(modetest_elf libdrm_fourcc_table)

# LINK_LIBS drm c → add_user_dyn_elf 内部自动 add_dependencies(drm_so)。
# NO_IMAGE: manual diagnostic ELF, not in test_runner's table.
add_user_dyn_elf(modetest_dyn C SOURCES modetest_dyn.c LINK_LIBS drm c NO_IMAGE)

# libudev shim 阶段 A 回归测试（TEST 构建——依赖 evdev 真实注册态）。
# 仿 test_sysfs：TEST 门控 + udev-shim 头文件 include 路径。
# SOURCES 直接编入 udev.c：udev_* 符号实现于此文件，单独编译即可，无需
# 链接整个 input 静态库（避免拖入 libinput 全套依赖）。默认 -Iinclude/uapi
# 已覆盖 udev.c 的 <xos/input.h>/<xos/ioctl.h>。
if(TEST)
    add_user_dyn_elf(test_libudev C SOURCES test_libudev.c ${CMAKE_SOURCE_DIR}/user/lib/udev-shim/udev.c LINK_LIBS c STATIC_LIBS unity DEFS TEST INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/user/lib/udev-shim)
endif()

# udevd daemon 通道端到端测试(TEST 构建——依赖 udevd 真实运行态 + evdev 注册态)。
# 仿 test_libudev:TEST 门控 + udev-shim 头文件 include 路径。
# test_udevd 直接调 udev_* 符号 → SOURCES 编入 udev.c(避免拖整个 input 静态库)。
if(TEST)
    add_user_dyn_elf(test_udevd C SOURCES test_udevd.c ${CMAKE_SOURCE_DIR}/user/lib/udev-shim/udev.c LINK_LIBS c STATIC_LIBS unity DEFS TEST INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/user/lib/udev-shim)
endif()

# udevd db + 规则引擎端到端测试(TEST 构建——依赖 udevd 真实运行态 + evdev 注册态)。
# 仿 test_libudev:TEST 门控 + udev-shim 头文件 include 路径。
# test_udevd_db 直接调 udev_* 符号 → SOURCES 编入 udev.c。
if(TEST)
    add_user_dyn_elf(test_rename C SOURCES test_rename.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_udevd_db C SOURCES test_udevd_db.c ${CMAKE_SOURCE_DIR}/user/lib/udev-shim/udev.c LINK_LIBS c STATIC_LIBS unity DEFS TEST INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/user/lib/udev-shim)
endif()

# devtmpfs 设备表动态化回归(work2_design §5.5.1)— TEST 门控,不调 udev_*,
# 只调 sys_dev_create/open/opendir(走 libc + syscall),无需编入 udev.c。
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
    # path-based inode 元数据/链接 syscall(§3.2/§3.5):access/faccessat 按 euid
    # 判定(Q4);utimensat 内存时间戳(Q5)。对齐 test_stat_real 风格,FAT32+tmpfs
    # 双夹具。阶段 3 link 部分经 TEST_LINK 启用(见 test_link_utimensat.c)。
    add_user_dyn_elf(test_access C SOURCES test_access.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_link_utimensat C SOURCES test_link_utimensat.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # §3.3 symlink/readlink(tmpfs 真实现,Q2):round-trip / lstat vs stat /
    # EEXIST / FAT32 ENOSYS / readlink 非 softlink EINVAL。对齐 test_stat_real。
    add_user_dyn_elf(test_symlink C SOURCES test_symlink.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # §3.4 link/linkat(tmpfs 硬链 + nlink 全链路,Q3):data 共享 / unlink 保
    # 数据 nlink-- / 目录 EPERM / 跨 fs EXDEV / FAT32 EPERM / EEXIST / flags EINVAL。
    add_user_dyn_elf(test_link C SOURCES test_link.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # chmod/chown 系(阶段1 capable 收口):mode/uid/gid 真写 inode + setuid 位清除
    # 规则(apply_chmod/apply_chown,root CAP_FSETID 保留 / 非特权清)。落盘仅内存
    # (与 utimensat 一致)。非 root 用例经 fork 子 setuid(1000)+_exit 码 + 父 waitpid。
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
    # execve S_ISUID/S_ISGID 凭证切换验证：setuid_helper.elf 用退出码报
    # geteuid/getegid/getuid，主测试 chmod 04755+chown(1000,1000) 后 fork 子
    # setuid(2000) drop root 再 execve helper，断 euid 切到 1000、real uid 保持。
    # 兼带 setuid 阶梯 gate 自洽化(阶段0 capable(CAP_SETUID))回归。
    add_user_dyn_elf(setuid_helper C SOURCES setuid_helper.c LINK_LIBS c)
    add_user_dyn_elf(test_setuid_exec C SOURCES test_setuid_exec.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    # M1.1 (unistd.md §2.A1/A2): setresuid/setresgid/setreuid/setregid +
    # getgroups. Permission ladder + -1 unchanged + suid-preservation rule
    # (man setresuid(2)/setreuid(2)), process-wide semantics. Non-root cases via
    # fork child setuid/setresuid(1000) + _exit code + parent waitpid.
    add_user_dyn_elf(test_setxid C SOURCES test_setxid.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
    add_user_dyn_elf(test_execve_vfs C SOURCES test_execve_vfs.c LINK_LIBS c STATIC_LIBS unity DEFS TEST)
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
# LINK_LIBS ffi c → add_user_dyn_elf 内部自动 add_dependencies(ffi_so/c_so)；
# GEN_HEADERS 由各对象 custom-command 的 DEPENDS 追踪（编辑模板触发重编）。

# expat smoke test: XML_ParserCreate / Parse / Free / error handling
add_user_dyn_elf(test_expat C SOURCES test_expat.c
    LINK_LIBS expat c
    STATIC_LIBS unity
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/libexpat/expat/lib
                ${CMAKE_BINARY_DIR})

# libwayland-client.so 纯逻辑测试: wl_array (init/add/for_each/copy/release) +
# wl_list (init/insert/remove/length/empty/insert_list). 仅覆盖不依赖真实
# compositor 的容器层; wl_display_*/wl_proxy 路径属 e2e, 不在本测范围.
# LINK_LIBS wayland-client → -lwayland-client resolves to libwayland-client.so
# (DT_NEEDED). INCLUDE_DIRS 同 wayland_client_so target 的取头路径:
# wayland-client.h (src) → wayland-client-protocol.h (build, 生成).
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
