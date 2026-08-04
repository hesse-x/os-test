/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "shell_command.h"

// Block until stdin is readable, then read one canonical line into buf
// (NUL-terminated, trailing newline stripped). Returns the byte count, or -1
// on EOF / read error. The M2-B self-pipe is drained by shell_wait_input
// before each read so a background child exit wakes the loop even while idle.
static int readline(char *buf, int len) {
  for (;;) {
    shell_wait_input();
    ssize_t n = read(0, buf, (size_t)(len - 1));
    if (n < 0) {
      if (errno == EINTR)
        continue; // SA_RESTART normally prevents this; be defensive
      // If foreground ownership was lost during a child state transition,
      // reclaim the terminal and retry instead of treating EIO as EOF.
      if (errno == EIO && tcsetpgrp(0, getpgrp()) == 0)
        continue;
      return -1;
    }
    if (n == 0)
      return -1; // EOF (master closed) — caller will HUP+exit, not busy-loop
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      buf[--n] = '\0';
    return (int)n;
  }
}

// ===================== Main =====================

// extern "C": clang under -ffreestanding mangles a C++ `main` (no hosted entry
// point concept), which breaks the crt0.o `main` reference. gcc leaves `main`
// unmangled regardless, so this is a no-op for gcc and required for clang.
extern "C" int main(int argc, char **argv, char **envp) {
  (void)envp;
  if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
    // A -c shell launched on a controlling tty still needs foreground process
    // groups (notably for terminal-generated SIGHUP/SIGINT semantics).
    if (isatty(0))
      shell_init_jobcontrol();
    return shell_run_command(argv[2]);
  }
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
    if (test_pid < 0) {
      perror("shell: failed to start test runner");
      return 1;
    }
    int test_status = 0;
    pid_t waited;
    do {
      waited = waitpid(test_pid, &test_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
      perror("shell: failed to wait for test runner");
    printf("shell: test runner finished status=%d, exiting test session\n",
           test_status);
    return 0;
  }
#endif

  char line[256];

  while (1) {
    // Drain background state changes before printing the prompt so a
    // finished job's "[jid]+ Done" notice appears above the prompt.
    shell_reap_jobs();
    printf("> ");
    // stdout is line-buffered on a tty: the prompt has no newline, so flush
    // it explicitly or it stays stuck until the next command's output.
    fflush(stdout);
    int len = readline(line, sizeof(line));
    if (len < 0) {
      // EOF (master closed): HUP every job and exit — do not busy-loop on a
      // disconnected slave.
      shell_hangup_jobs();
      return 0;
    }
    if (len == 0)
      continue;

    // Unified executor: the same parser/pipeline engine as `sh -c`
    // (step1.md §3.5). cd/exit/export/unset run in the parent; everything
    // else runs in pipeline children with a foreground pgid.
    shell_run_command(line);
    int ex = shell_requested_exit();
    if (ex >= 0) {
      shell_hangup_jobs();
      return ex;
    }
  }
}
