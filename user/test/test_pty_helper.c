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
//             for master-close (SIGHUP). The launching shell performs the
//             foreground handoff before waiting; calling tcsetpgrp here would
//             race that handoff and can stop us with SIGTTOU. The READY marker
//             on stderr is visible even for the first pipeline stage.
//   --winch   install a SIGWINCH handler printing "WINCH", take the terminal
//             as foreground, print "WINCH_READY", then block. The readiness
//             marker guarantees the handler is installed before the test
//             issues TIOCSWINSZ (SIGWINCH defaults to ignore otherwise).
//   <else>    echo argv[1..] joined by '|' to stdout, exit 0 (argv/PATH test).
//
// Packaged as /test/test_pty_helper.elf and /usr/bin/ptytest.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static void on_winch(int sig) {
  (void)sig;
  write(1, "WINCH\n", 6);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--loop")) {
    write(2, "READY\n", 6);
    for (;;)
      pause();
  }
  if (argc > 1 && !strcmp(argv[1], "--winch")) {
    signal(SIGWINCH, on_winch);
    write(2, "WINCH_READY\n", 12);
    for (;;)
      pause();
  }
  // M2-A: background slave read -> SIGTTIN (stop); resume on fg+SIGCONT.
  // The helper is the forkpty session leader (ctty, foreground). It forks a
  // background child (own pgid); the child's read(0) is gated by the kernel
  // foreground-access rule -> SIGTTIN -> stop. The helper then hands the tty
  // to the child + SIGCONT so the restarted read collects the byte the test
  // driver injects on the master. SIGTTOU is ignored so the helper itself is
  // not stopped when it calls tcsetpgrp from the background.
  if (argc > 1 && !strcmp(argv[1], "--bgread")) {
    signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(0, getpgrp());
    pid_t bg = fork();
    if (bg == 0) {
      setpgid(0, 0); // own group = background
      char c = 0;
      ssize_t n = read(0, &c, 1);
      if (n == 1)
        dprintf(2, "BGREAD:%c\n", c);
      _exit(0);
    }
    setpgid(bg, bg);
    int st = 0;
    waitpid(bg, &st, WUNTRACED);
    dprintf(2, "BGSTOPPED:%d\n", WIFSTOPPED(st) ? WSTOPSIG(st) : -1);
    tcsetpgrp(0, bg);  // make the bg child foreground
    kill(bg, SIGCONT); // resume its -ERESTART'd read
    int st2 = 0;
    waitpid(bg, &st2, 0);
    tcsetpgrp(0, getpgrp());
    dprintf(2, "BGEXIT:%d\n", WIFEXITED(st2) ? WEXITSTATUS(st2) : -1);
    _exit(0);
  }
  // M2-A: background slave write. Without TOSTOP the write succeeds; with
  // TOSTOP set (by the foreground helper before forking the bg child) the
  // child's write is gated -> SIGTTOU -> stop.
  if (argc > 1 && !strcmp(argv[1], "--bgwrite")) {
    int tostop = (argc > 2 && !strcmp(argv[2], "tostop"));
    if (tostop) {
      struct termios t;
      tcgetattr(0, &t);
      t.c_lflag |= TOSTOP;
      tcsetattr(0, TCSANOW, &t);
    }
    tcsetpgrp(0,
              getpgrp()); // foreground; SIGTTOU stays default so the bg
                          // child inherits default and is stopped by TOSTOP
    pid_t bg = fork();
    if (bg == 0) {
      setpgid(0, 0); // background; SIGTTOU default (stop)
      ssize_t n = write(1, "BGWROTE\n", 8);
      if (n < 0)
        dprintf(2, "BGWRITE_ERR:%d\n", errno);
      _exit(0);
    }
    setpgid(bg, bg);
    int st = 0;
    waitpid(bg, &st, WUNTRACED);
    if (WIFSTOPPED(st)) {
      dprintf(2, "BGSTOPPED_W:%d\n", WSTOPSIG(st));
      kill(bg, SIGKILL);
      waitpid(bg, &st, 0);
    } else if (WIFEXITED(st)) {
      dprintf(2, "BGWEXIT:%d\n", WEXITSTATUS(st));
    }
    if (tostop) {
      struct termios t;
      tcgetattr(0, &t);
      t.c_lflag &= ~TOSTOP;
      tcsetattr(0, TCSANOW, &t);
    }
    _exit(0);
  }
  // M2-A: tcsetpgrp errno matrix.
  //  R1: non-ctty fd (a pipe) -> ENOTTY
  //  R2: nonexistent pgid on the ctty -> EPERM
  //  R3: a child in a different session (setsid) -> ENOTTY (ctty cleared /
  //      t_sid != caller sid)
  if (argc > 1 && !strcmp(argv[1], "--ttioerrs")) {
    int pp[2];
    pipe(pp);
    errno = 0;
    int r1 = tcsetpgrp(pp[0], getpgrp());
    dprintf(2, "R1:%d:%d\n", r1, errno);
    close(pp[0]);
    close(pp[1]);

    errno = 0;
    int r2 = tcsetpgrp(0, 999999);
    dprintf(2, "R2:%d:%d\n", r2, errno);

    pid_t c = fork();
    if (c == 0) {
      setsid();
      errno = 0;
      int r = tcsetpgrp(0, getpgrp());
      dprintf(2, "R3:%d:%d\n", r, errno);
      _exit(0);
    }
    int cst = 0;
    waitpid(c, &cst, 0);
    _exit(0);
  }
  for (int i = 1; i < argc; i++)
    printf("%s%s", i == 1 ? "" : "|", argv[i]);
  putchar('\n');
  return 0;
}
