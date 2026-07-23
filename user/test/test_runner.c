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
    {"string", "/test/string.elf"},
    {"malloc", "/test/malloc.elf"},
    {"stdio", "/test/stdio.elf"},
    {"mmap", "/test/mmap.elf"},
    {"ipc", "/test/ipc.elf"},
    {"socket", "/test/socket.elf"},
    {"process", "/test/process.elf"},
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
    {"hello_dyn", "/local/hello_dyn.elf"},
    {"ld_single", "/test/ld_test_single.elf"},
    {"ld_chain", "/test/ld_test_chain.elf"},
    {"ld_diamond", "/test/ld_test_diamond.elf"},
    {"ld_cycle", "/test/ld_test_cycle.elf"},
    {"drm_ioctl", "/test/drm_ioctl.elf"},
    {"drm_phase_c", "/test/drm_phase_c.elf"},
    {"drm_test_link", "/test/drm_test_link.elf"},
    {"venus_channel", "/test/venus_channel.elf"},
#ifdef TEST
    {"test_sysfs", "/test/test_sysfs.elf"},
    {"test_libudev", "/test/test_libudev.elf"},
    {"test_vfs_dispatch", "/test/test_vfs_dispatch.elf"},
    {"test_inode_refcount", "/test/test_inode_refcount.elf"},
    {"test_tmpfs_socket", "/test/test_tmpfs_socket.elf"},
    {"test_rename", "/test/test_rename.elf"},
    {"test_dirent_seek", "/test/test_dirent_seek.elf"},
    {"test_cloexec_perfd", "/test/test_cloexec_perfd.elf"},
    {"test_openat_dirfd", "/test/test_openat_dirfd.elf"},
    {"test_stat_real", "/test/test_stat_real.elf"},
    {"test_udevd_db", "/test/test_udevd_db.elf"},
    {"test_udevd", "/test/test_udevd.elf"},
    {"test_dev_vfs_dynamic", "/test/test_dev_vfs_dynamic.elf"},
    {"test_mprotect", "/test/test_mprotect.elf"},
    {"test_vma_restructure", "/test/test_vma_restructure.elf"},
    {"test_mmap_addr_hint", "/test/test_mmap_addr_hint.elf"},
    {"test_mmap_file_private", "/test/test_mmap_file_private.elf"},
    {"test_ffi", "/test/test_ffi.elf"},
#endif
    {"ioctl_varlen", "/test/ioctl_varlen.elf"},
    {"epoll", "/test/epoll.elf"},
    {"eventfd", "/test/eventfd.elf"},
    {"timerfd", "/test/timerfd.elf"},
    {"signalfd", "/test/signalfd.elf"},
    {"getrandom", "/test/getrandom.elf"},
    {"mount", "/test/mount.elf"},
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  printf("=== Test Runner ===\n");

  int pass_count = 0;
  int fail_count = 0;
  int skip_count = 0;

  for (size_t i = 0; i < NUM_TESTS; i++) {
    const char *name = tests[i].name;
    const char *path = tests[i].path;

    printf("[RUN]  %-20s ... running\n", name);

    pid_t pid = spawn(path);
    if (pid <= 0) {
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
