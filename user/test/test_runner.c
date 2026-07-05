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
}
