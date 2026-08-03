/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "shell_command.h"

static int readline(char *buf, int len) {
  // Canonical-mode read: the kernel line discipline buffers the line and
  // echoes it (step1.md §3.4); a single read returns one complete line.
  ssize_t n = read(0, buf, (size_t)(len - 1));
  if (n <= 0)
    return 0;
  buf[n] = '\0';
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = '\0';
  return (int)n;
}

// ===================== Main =====================

// extern "C": clang under -ffreestanding mangles a C++ `main` (no hosted entry
// point concept), which breaks the crt0.o `main` reference. gcc leaves `main`
// unmangled regardless, so this is a no-op for gcc and required for clang.
extern "C" int main(int argc, char **argv, char **envp) {
  (void)envp;
  if (argc >= 3 && strcmp(argv[1], "-c") == 0)
    return shell_run_command(argv[2]);
  if (argc != 1) {
    fprintf(stderr, "usage: sh [-c command]\n");
    return 2;
  }
  // VFS is in-kernel, no need to wait for fs_driver
  printf("shell: ready\n");

  // Become session leader and set controlling terminal (the forkpty child
  // already did this via libc login_tty; these are idempotent re-statements).
  setsid();
  ioctl(0, TIOCSCTTY, 0);
  shell_init_jobcontrol();

#ifdef TEST
  // Shells launched by integration tests must not recursively start the
  // complete test suite inside their newly-created PTY.
  if (getenv("XOS_SKIP_AUTOTEST") == nullptr) {
    pid_t test_pid = fork();
    if (test_pid == 0) {
      execve("/test/test_runner.elf", NULL, NULL);
      _exit(127);
    }
    int test_status = 0;
    waitpid(test_pid, &test_status, 0);
  }
#endif

  char line[256];

  while (1) {
    printf("> ");
    // stdout is line-buffered on a tty: the prompt has no newline, so flush
    // it explicitly or it stays stuck until the next command's output.
    fflush(stdout);
    int len = readline(line, sizeof(line));
    if (len == 0)
      continue;

    // Unified executor: the same parser/pipeline engine as `sh -c`
    // (step1.md §3.5). cd/exit/export/unset run in the parent; everything
    // else runs in pipeline children with a foreground pgid.
    shell_run_command(line);
    int ex = shell_requested_exit();
    if (ex >= 0)
      return ex;
  }
}
