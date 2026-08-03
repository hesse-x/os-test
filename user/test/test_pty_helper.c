/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_pty_helper — companion ELF for test_pty.c (PTY vertical integration,
// terminal/step1.md §6.2). Modes selected by argv[1]:
//
//   --loop    take the terminal as foreground, print "READY" on stderr, then
//             block forever on pause(). Foreground target for ^C (SIGINT) and
//             for master-close (SIGHUP). The explicit tcsetpgrp removes the
//             race where the shell's handoff has not happened yet, and the
//             READY marker on stderr is visible on the master even for the
//             first stage of a pipeline (only stdout is redirected).
//   --winch   install a SIGWINCH handler printing "WINCH", take the terminal
//             as foreground, print "WINCH_READY", then block. The readiness
//             marker guarantees the handler is installed before the test
//             issues TIOCSWINSZ (SIGWINCH defaults to ignore otherwise).
//   <else>    echo argv[1..] joined by '|' to stdout, exit 0 (argv/PATH test).
//
// Packaged as /test/test_pty_helper.elf and /usr/bin/ptytest.

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void on_winch(int sig) {
  (void)sig;
  write(1, "WINCH\n", 6);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--loop")) {
    tcsetpgrp(0, getpgrp()); /* job leader: guarantee t_pgid == our group */
    write(2, "READY\n", 6);
    for (;;)
      pause();
  }
  if (argc > 1 && !strcmp(argv[1], "--winch")) {
    signal(SIGWINCH, on_winch);
    tcsetpgrp(0, getpgrp());
    write(2, "WINCH_READY\n", 12);
    for (;;)
      pause();
  }
  for (int i = 1; i < argc; i++)
    printf("%s%s", i == 1 ? "" : "|", argv[i]);
  putchar('\n');
  return 0;
}
