/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct test_entry {
  const char *name;
  const char *path;
};

static struct test_entry tests[] = {
    {"pipe", "/test/pipe.elf"},
    {"fcntl", "/test/fcntl.elf"},
    {"fcntl_ofd", "/test/fcntl_ofd.elf"},
    {"flock", "/test/flock.elf"},
    {"accept4", "/test/accept4.elf"},
    {"string", "/test/string.elf"},
    {"malloc", "/test/malloc.elf"},
    {"stdio", "/test/stdio.elf"},
    {"tmpfile", "/test/tmpfile.elf"},
    {"mmap", "/test/mmap.elf"},
    {"ipc", "/test/ipc.elf"},
    {"socket", "/test/socket.elf"},
    {"socket_msgflags", "/test/test_socket_msgflags.elf"},
    {"unix_dgram", "/test/test_unix_dgram.elf"},
    {"process", "/test/process.elf"},
    {"spawn_popen", "/test/test_spawn_popen.elf"},
    {"signal", "/test/signal.elf"},
    {"signal_stop", "/test/signal_stop.elf"},
    {"signal_flags", "/test/signal_flags.elf"},
    {"kill_perm", "/test/kill_perm.elf"},
    {"sigaltstack", "/test/sigaltstack.elf"},
    {"poll", "/test/poll.elf"},
    {"pci", "/test/pci.elf"},
    {"ioctl", "/test/ioctl.elf"},
    {"dev_vfs", "/test/dev_vfs.elf"},
    {"fpu", "/test/test_fpu.elf"},
    {"sse_smoke", "/test/test_sse_smoke.elf"},
    {"pthread", "/test/pthread.elf"},
    {"c11threads", "/test/c11threads.elf"},
    {"sched", "/test/test_sched.elf"},
    {"clock_realtime", "/test/test_clock_realtime.elf"},
    {"time_sleep", "/test/test_time_sleep.elf"},
    {"clock_cputime", "/test/test_clock_cputime.elf"},
    {"membarrier", "/test/test_membarrier.elf"},
    {"test_inttypes", "/test/test_inttypes.elf"},
    {"test_musl_misc", "/test/test_musl_misc.elf"},
    {"test_misc", "/test/test_misc.elf"},
    {"test_console", "/test/test_console.elf"},
    {"test_locale", "/test/test_locale.elf"},
    {"test_regex", "/test/test_regex.elf"},
    {"test_passwd", "/test/test_passwd.elf"},
#if defined(TEST)
    {"egl_smoke", "/test/test_egl_smoke.elf"},
    /* wlroots prerequisite dep runtime smoke (WF-3/WF-5). Staged .so come
     * from `build.sh --wlroots`; pure-logic, no devices/seatd. libseat is
     * deferred to WF-6. */
    {"pixman_smoke", "/test/test_pixman_smoke.elf"},
    {"display_info_smoke", "/test/test_display_info_smoke.elf"},
    {"xkbcommon_smoke", "/test/test_xkbcommon_smoke.elf"},
#endif
    {"hello_dyn", "/local/hello_dyn.elf"},
    /* ldso.md Phase 1.5 go/no-go: musl fused libc.so self-bootstraps as
     * PT_INTERP /lib/ld-musl-x86_64.so.1 and jumps to main. The ld_* ELFs pull
     * liba.so/ libb.so from /test/lib via LD_LIBRARY_PATH (see child_env in
     * main). */
    {"ld_single", "/test/ld_test_single.elf"},
    {"ld_chain", "/test/ld_test_chain.elf"},
    {"ld_diamond", "/test/ld_test_diamond.elf"},
    {"ld_cycle", "/test/ld_test_cycle.elf"},
    /* dlfcn API smoke (DL-001~006): dlopen(NULL)/dlsym/dlclose/dlerror/dladdr
     * against the fused loader + musl_dl_objs wrappers. Not a real .so load
     * (Phase 3) — dlopen(NULL) returns the head handle, dlsym resolves against
     * the existing global chain. */
    {"test_dl", "/test/test_dl.elf"},
    {"drm_ioctl", "/test/drm_ioctl.elf"},
    {"drm_phase_c", "/test/drm_phase_c.elf"},
    {"drm_test_link", "/test/drm_test_link.elf"},
    {"virgl_channel", "/test/virgl_channel.elf"},
    {"test_sysfs", "/test/test_sysfs.elf"},
    {"test_procfs", "/test/test_procfs.elf"},
    {"test_libudev", "/test/test_libudev.elf"},
    {"test_vfs_dispatch", "/test/test_vfs_dispatch.elf"},
    {"test_inode_refcount", "/test/test_inode_refcount.elf"},
    {"test_tmpfs_socket", "/test/test_tmpfs_socket.elf"},
    {"test_rename", "/test/test_rename.elf"},
    {"test_dirent_seek", "/test/test_dirent_seek.elf"},
    {"test_dirent_complete", "/test/test_dirent_complete.elf"},
    {"test_resource", "/test/test_resource.elf"},
    {"test_cloexec_perfd", "/test/test_cloexec_perfd.elf"},
    {"test_pipe2", "/test/test_pipe2.elf"},
    {"test_openat_dirfd", "/test/test_openat_dirfd.elf"},
    {"test_stat_real", "/test/test_stat_real.elf"},
    {"test_statx", "/test/test_statx.elf"},
    {"test_access", "/test/test_access.elf"},
    {"test_eaccess", "/test/test_eaccess.elf"},
    {"test_link_utimensat", "/test/test_link_utimensat.elf"},
    {"test_symlink", "/test/test_symlink.elf"},
    {"test_link", "/test/test_link.elf"},
    {"test_chmod", "/test/test_chmod.elf"},
    {"test_udevd_db", "/test/test_udevd_db.elf"},
    {"test_udevd", "/test/test_udevd.elf"},
    {"test_dev_vfs_dynamic", "/test/test_dev_vfs_dynamic.elf"},
    {"test_mprotect", "/test/test_mprotect.elf"},
    {"test_vma_restructure", "/test/test_vma_restructure.elf"},
    {"test_mmap_addr_hint", "/test/test_mmap_addr_hint.elf"},
    {"test_mmap_file_private", "/test/test_mmap_file_private.elf"},
    {"test_munmap_partial", "/test/test_munmap_partial.elf"},
    {"test_mprotect_partial", "/test/test_mprotect_partial.elf"},
    {"test_mmap_flags", "/test/test_mmap_flags.elf"},
    {"test_mmap_size_limit", "/test/test_mmap_size_limit.elf"},
    {"clone_exit_signal", "/test/test_clone_exit_signal.elf"},
    {"clone_settid_fault", "/test/test_clone_settid_fault.elf"},
    {"wait4_pgid_rusage", "/test/test_wait4_pgid_rusage.elf"},
    {"wait4_options", "/test/test_wait4_options.elf"},
    {"setuid_saved", "/test/test_setuid_saved.elf"},
    {"setuid_exec", "/test/test_setuid_exec.elf"},
    {"setxid", "/test/test_setxid.elf"},
    {"execve_vfs", "/test/test_execve_vfs.elf"},
    {"test_ffi", "/test/test_ffi.elf"},
    {"test_expat", "/test/test_expat.elf"},
    {"test_wayland_client", "/test/test_wayland_client.elf"},
#if defined(LIBCXX)
    /* libc++ end-to-end regression (refact_cmake.md §3.6): vector/string,
     * throw/catch (libc++abi TLS), filesystem (int128 runtime), messages facet
     * (catgets). Only present when built with -DLIBCXX=1 (./build.sh --cxx). */
    {"libcxx_smoke", "/test/libcxx_smoke.elf"},
#endif
    {"ioctl_varlen", "/test/ioctl_varlen.elf"},
    {"epoll", "/test/epoll.elf"},
    {"epoll_oneshot", "/test/test_epoll_oneshot.elf"},
    {"eventfd", "/test/eventfd.elf"},
    {"timerfd", "/test/timerfd.elf"},
    {"signalfd", "/test/signalfd.elf"},
    {"getrandom", "/test/getrandom.elf"},
    {"mount", "/test/mount.elf"},
    /* S18 accept_no_timeout sleeps up to 35s by design — run last so it does
     * not gate the rest of the suite on its deliberate long block. */
    {"getdents_resume", "/test/test_getdents_resume.elf"},
    {"sa_restart", "/test/test_sa_restart.elf"},
    {"sa_nocldwait", "/test/test_sa_nocldwait.elf"},
    {"accept_no_timeout", "/test/test_accept_no_timeout.elf"},
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  printf("=== Test Runner ===\n");

  // Child search path for the dynamic loader: /lib (libc.so / ld-musl) plus
  // /test/lib (the ld_* chain/diamond/cycle stub libs liba.so/libb.so, shipped
  // there by os_image_path). The kernel propagates envp to the new image and
  // the musl loader reads LD_LIBRARY_PATH, so this is enough — no RPATH and no
  // global loader config change needed. We can't use spawn() because it passes
  // a NULL envp (empty environment, envc=0), so fork+execve with this env.
  char *child_env[] = {"LD_LIBRARY_PATH=/lib:/test/lib", NULL};

  int pass_count = 0;
  int fail_count = 0;
  int skip_count = 0;

  for (size_t i = 0; i < NUM_TESTS; i++) {
    const char *name = tests[i].name;
    const char *path = tests[i].path;

    printf("[RUN]  %-20s ... running\n", name);

    pid_t pid = fork();
    if (pid == 0) {
      execve(path, NULL, child_env);
      _exit(127);
    }
    if (pid < 0) {
      printf("[SKIP] %-20s (cannot spawn)\n", name);
      skip_count++;
      continue;
    }

    int status;
    waitpid(pid, &status, 0);

    if (status == 0) {
      printf("[PASS] %-20s (exit 0)\n", name);
      pass_count++;
    } else {
      printf("[FAIL] %-20s (exit %d) -- check serial log\n", name, status);
      fail_count++;
    }
  }

  printf("=== Summary: PASS=%d FAIL=%d SKIP=%d ===\n", pass_count, fail_count,
         skip_count);

  _exit(fail_count > 0 ? 1 : 0);
  return 0;
}
