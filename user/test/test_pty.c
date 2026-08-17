/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// PTY vertical integration test (terminal/step1.md §6.2). No graphics
// dependency: drives /dev/pts pairs via openpty/forkpty and asserts on the
// kernel N_TTY line discipline (kernel/bsd/pty.c) end to end — from raw
// ldisc editing up through the interactive shell's job control.
//
// Two failure-modes matter beyond the assertions: every read is bounded by a
// timeout (pty_wait_for), and alarm(90) caps the whole ELF so a genuinely
// stuck kernel/shell path cannot hang the test_runner.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// ---- accumulator helpers --------------------------------------------------
// The master fd is set O_NONBLOCK; reads accumulate into a sliding buffer so
// markers can be searched for across arbitrary kernel/echo interleavings.

#define ACC_CAP 8192

static void acc_append(char *acc, int *accn, const char *buf, int n) {
  for (int i = 0; i < n; i++) {
    if (*accn >= ACC_CAP - 1) {
      int half = ACC_CAP / 2;
      memmove(acc, acc + half, (size_t)(ACC_CAP - half));
      *accn -= half;
    }
    acc[(*accn)++] = buf[i];
  }
  acc[*accn] = '\0';
}

static void msleep(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000,
                        .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// Read from fd (nonblocking) until `needle` is a substring of the accumulator.
// Returns 1 on match, 0 on timeout.
static int pty_wait_for(int fd, char *acc, int *accn, const char *needle,
                        int timeout_ms) {
  int waited = 0;
  while (waited < timeout_ms) {
    acc[*accn] = '\0';
    if (strstr(acc, needle))
      return 1;
    char tmp[512];
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n > 0) {
      acc_append(acc, accn, tmp, (int)n);
      continue;
    }
    msleep(20);
    waited += 20;
  }
  acc[*accn] = '\0';
  return strstr(acc, needle) != NULL;
}

// Reap pid with a timeout: force-kill once the child outlives it, so a stuck
// forkpty child never hangs the runner and never leaks (Unity asserts longjmp
// past cleanup, so every long-running case must do its own bounded reap).
static void pty_reap(pid_t pid, int timeout_ms, int *status) {
  int waited = 0;
  for (;;) {
    pid_t w = waitpid(pid, status, WNOHANG);
    if (w == pid || w < 0)
      return;
    if (waited >= timeout_ms)
      kill(pid, SIGKILL);
    msleep(50);
    waited += 50;
  }
}

// pty_wait_for variant: wait until the accumulator contains `n` occurrences
// of the literal "READY" (one per foreground job stage).
static int pty_wait_ready(int fd, char *acc, int *accn, int n, int timeout_ms) {
  int waited = 0;
  while (waited < timeout_ms) {
    acc[*accn] = '\0';
    int count = 0;
    for (const char *p = acc; (p = strstr(p, "READY")) != NULL; p += 5)
      count++;
    if (count >= n)
      return 1;
    char tmp[512];
    ssize_t r = read(fd, tmp, sizeof(tmp));
    if (r > 0) {
      acc_append(acc, accn, tmp, (int)r);
      continue;
    }
    msleep(20);
    waited += 20;
  }
  return 0;
}

// ---- case 1: openpty/forkpty/ptsname_r/ttyname_r (M0) ---------------------
void test_pty_openpty_names(void) {
  char name[64];
  int m, s;
  TEST_ASSERT_EQUAL_INT(0, openpty(&m, &s, name, NULL, NULL));
  TEST_ASSERT_TRUE(strncmp(name, "/dev/pts/", 9) == 0);

  char name2[64];
  TEST_ASSERT_EQUAL_INT(0, ptsname_r(m, name2, sizeof(name2)));
  TEST_ASSERT_EQUAL_STRING(name, name2);

  char name3[64];
  TEST_ASSERT_EQUAL_INT(0, ttyname_r(s, name3, sizeof(name3)));
  TEST_ASSERT_EQUAL_STRING(name, name3);

  int idx = -1;
  TEST_ASSERT_EQUAL_INT(0, ioctl(m, TIOCGPTN, &idx));
  TEST_ASSERT_TRUE(idx >= 0);

  close(m);
  close(s);
}

void test_pty_forkpty_roundtrip(void) {
  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", "echo pty-fork-ok", (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  TEST_ASSERT_TRUE(pty_wait_for(master, acc, &accn, "pty-fork-ok", 8000));
  int status = 0;
  pty_reap(pid, 4000, &status);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  close(master);
}

// ---- case 2: 16 PTY create/destroy, node disappears, no leak (M0) ---------
void test_pty_16_create_destroy(void) {
  struct stat st;
  for (int i = 0; i < 16; i++) {
    char name[64];
    int m, s;
    TEST_ASSERT_EQUAL_INT(0, openpty(&m, &s, name, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, stat(name, &st));
    close(s); // slave close removes the devtmpfs node
    TEST_ASSERT_TRUE(stat(name, &st) != 0);
    close(m);
  }
  // A fresh pair still allocates: indices were recycled, nothing leaked.
  char name[64];
  int m, s;
  TEST_ASSERT_EQUAL_INT(0, openpty(&m, &s, name, NULL, NULL));
  close(s);
  close(m);
}

// ---- case 3: env inheritance through forkpty exec (M0) --------------------
void test_pty_env_inherit(void) {
  setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
  setenv("HOME", "/root", 1);
  setenv("TERM", "xterm-256color", 1);

  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", "echo \"PATH=$PATH HOME=$HOME TERM=$TERM\"",
          (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  TEST_ASSERT_TRUE(pty_wait_for(master, acc, &accn,
                                "PATH=/usr/local/bin:/usr/bin:/bin", 8000));
  TEST_ASSERT_TRUE(strstr(acc, "HOME=/root") != NULL);
  TEST_ASSERT_TRUE(strstr(acc, "TERM=xterm-256color") != NULL);
  int status = 0;
  pty_reap(pid, 4000, &status);
  close(master);
}

// ---- case 4: canonical editing abc<BS>d<Enter> -> "abd\n" (M1) -----------
void test_pty_ldisc_erase(void) {
  int m, s;
  TEST_ASSERT_EQUAL_INT(0, openpty(&m, &s, NULL, NULL, NULL));

  const char input[] = "abc\x7f"
                       "d\r";
  TEST_ASSERT_EQUAL_INT((int)sizeof(input) - 1,
                        (int)write(m, input, sizeof(input) - 1));

  // The committed line comes back edited.
  char line[64];
  ssize_t n = read(s, line, sizeof(line) - 1);
  TEST_ASSERT_EQUAL_INT(4, (int)n);
  line[n] = '\0';
  TEST_ASSERT_EQUAL_STRING("abd\n", line);

  // Echo reflects input + the ERASE seq; ICRNL makes the trailing \r echo
  // as \n (OPOST -> \r\n on the master).
  fcntl(m, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  TEST_ASSERT_TRUE(pty_wait_for(m, acc, &accn, "abc", 4000));
  TEST_ASSERT_TRUE(strstr(acc, "\b \b") != NULL);
  TEST_ASSERT_TRUE(strstr(acc, "d\r\n") != NULL);

  close(s);
  close(m);
}

void test_pty_ldisc_extended_editing(void) {
  int m, s;
  TEST_ASSERT_EQUAL_INT(0, openpty(&m, &s, NULL, NULL, NULL));

  struct termios t;
  TEST_ASSERT_EQUAL_INT(0, tcgetattr(s, &t));
  TEST_ASSERT_TRUE((t.c_lflag & (IEXTEN | ECHOCTL)) == (IEXTEN | ECHOCTL));
  TEST_ASSERT_EQUAL_HEX8(0x17, t.c_cc[VWERASE]);
  TEST_ASSERT_EQUAL_HEX8(0x12, t.c_cc[VREPRINT]);
  TEST_ASSERT_EQUAL_HEX8(0x16, t.c_cc[VLNEXT]);

  // ^W removes "two", ^R redraws without changing input, and ^V makes the
  // following ^D data instead of committing the line.
  const char input[] = "one two\x17X\x12!\x16\x04\r";
  TEST_ASSERT_EQUAL_INT((int)sizeof(input) - 1,
                        (int)write(m, input, sizeof(input) - 1));

  const char expected[] = "one X!\x04\n";
  char line[64];
  ssize_t n = read(s, line, sizeof(line));
  TEST_ASSERT_EQUAL_INT((int)sizeof(expected) - 1, (int)n);
  TEST_ASSERT_EQUAL_MEMORY(expected, line, sizeof(expected) - 1);

  fcntl(m, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  TEST_ASSERT_TRUE(pty_wait_for(m, acc, &accn, "^R\r\none X", 4000));
  TEST_ASSERT_TRUE(strstr(acc, "^D") != NULL);

  close(s);
  close(m);
}

// ---- case 6: ^D empty line = EOF, non-empty commits (M1) -----------------
void test_pty_ctrl_d_eof(void) {
  int m, s;
  TEST_ASSERT_EQUAL_INT(0, openpty(&m, &s, NULL, NULL, NULL));

  // Empty line + ^D: next read returns 0 (EOF).
  TEST_ASSERT_EQUAL_INT(1, (int)write(m, "\x04", 1));
  char buf[16];
  ssize_t n = read(s, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(0, (int)n);

  // Non-empty line + ^D: commits the bytes without a newline.
  TEST_ASSERT_EQUAL_INT(3, (int)write(m, "hi\x04", 3));
  n = read(s, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT(2, (int)n);
  buf[n] = '\0';
  TEST_ASSERT_EQUAL_STRING("hi", buf);

  close(s);
  close(m);
}

// ---- case 7: raw mode, each byte immediately readable, no echo (M1) ------
void test_pty_raw_mode(void) {
  int m, s;
  TEST_ASSERT_EQUAL_INT(0, openpty(&m, &s, NULL, NULL, NULL));

  struct termios t;
  TEST_ASSERT_EQUAL_INT(0, tcgetattr(s, &t));
  t.c_lflag &= ~(ICANON | ECHO);
  TEST_ASSERT_EQUAL_INT(0, tcsetattr(s, TCSANOW, &t));

  // VMIN=1 semantics: a byte is readable immediately.
  TEST_ASSERT_EQUAL_INT(1, (int)write(m, "a", 1));
  char ch;
  TEST_ASSERT_EQUAL_INT(1, (int)read(s, &ch, 1));
  TEST_ASSERT_EQUAL_INT('a', ch);

  // ECHO cleared: nothing lands on the master.
  fcntl(m, F_SETFL, O_NONBLOCK);
  char tmp[16];
  ssize_t r = read(m, tmp, sizeof(tmp));
  TEST_ASSERT_TRUE(r < 0); // -EAGAIN

  close(s);
  close(m);
}

// ---- case 5 (kernel): ^C delivers SIGINT to the foreground pgid ----------
void test_pty_ctrl_c_sigint(void) {
  int ready[2];
  TEST_ASSERT_EQUAL_INT(0, pipe(ready));
  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    // login_tty set t_pgid == our group; re-state it and tell the parent we
    // are the foreground group before it injects ^C.
    tcsetpgrp(0, getpgrp());
    char c = 1;
    write(ready[1], &c, 1);
    close(ready[1]);
    for (;;)
      pause();
  }
  close(ready[1]);
  char c;
  TEST_ASSERT_TRUE(read(ready[0], &c, 1) == 1);
  close(ready[0]);

  TEST_ASSERT_TRUE(write(master, "\x03", 1) == 1);
  int status = 0;
  pty_reap(pid, 4000, &status);
  TEST_ASSERT_TRUE(WIFSIGNALED(status));
  TEST_ASSERT_EQUAL_INT(SIGINT, WTERMSIG(status));
  close(master);
}

// ---- case 5 (shell) / 10: ^C kills only the foreground job; the shell
//      survives and reports the status via $? ------------------------------
static void interactive_ctrl_c(int use_pipe) {
  int master;
  // The interactive shell drives linenoise, whose getColumns() probes the
  // terminal width via TIOCGWINSZ and only falls back to an ESC[6n cursor
  // report when ws_col == 0. Over a bare PTY nothing answers that report, so
  // an uninitialised winsize (col 0) deadlocks the shell before it can draw
  // the prompt. Give the pty a sane 80x24 default so the ioctl path succeeds.
  struct winsize ws = {.ws_row = 24, .ws_col = 80};
  pid_t pid = forkpty(&master, NULL, NULL, &ws);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/bin/sh", "sh", (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';

  // Every wait/write is captured into a flag first and asserted only after
  // the child has been force-cleaned up — a stuck shell must not leak or
  // hang, it must only fail the assertion.

  // Prompt = the shell is back in its readline loop.
  int prompt_ok = pty_wait_for(master, acc, &accn, "> ", 8000);

  const char *cmd = use_pipe ? "/test/test_pty_helper.elf --loop | "
                               "/test/test_pty_helper.elf --loop\r"
                             : "/test/test_pty_helper.elf --loop\r";
  int write_ok =
      prompt_ok && write(master, cmd, strlen(cmd)) == (ssize_t)strlen(cmd);

  // Every job stage prints READY on stderr (the slave for pipeline stages —
  // pipes redirect only stdout), so this proves the whole job is up and its
  // leader has taken the terminal as foreground.
  int ready_ok = pty_wait_ready(master, acc, &accn, use_pipe ? 2 : 1, 8000);

  // ^C: ldisc flushes input, echoes ^C, SIGINTs the job pgid. The shell —
  // own group, SIGINT ignored — survives and regains the terminal. Wait for
  // its next prompt before sending the probe so the probe cannot race the
  // signal character's input flush.
  int ctrlc_ok = ready_ok && write(master, "\x03", 1) == 1;
  accn = 0;
  acc[0] = '\0';
  int reprompt_ok = ctrlc_ok && pty_wait_for(master, acc, &accn, "> ", 8000);
  int probe_ok = reprompt_ok && write(master, "echo $?\r", 8) == 8;
  int status_ok = probe_ok && pty_wait_for(master, acc, &accn, "130", 8000);

  close(master);
  int status = 0;
  pty_reap(pid, 4000, &status);

  TEST_ASSERT_TRUE(prompt_ok);
  TEST_ASSERT_TRUE(write_ok);
  TEST_ASSERT_TRUE(ready_ok);
  TEST_ASSERT_TRUE(ctrlc_ok);
  TEST_ASSERT_TRUE(reprompt_ok);
  TEST_ASSERT_TRUE(probe_ok);
  TEST_ASSERT_TRUE(status_ok);
}

void test_pty_ctrl_c_shell_survives(void) { interactive_ctrl_c(0); }

void test_pty_pipeline_ctrl_c(void) { interactive_ctrl_c(1); }

// ---- case 8: pipeline with redirect writes the file (M1) -----------------
void test_pty_pipe_redirect(void) {
  const char *path = "/local/pty-cat.txt";
  unlink(path);

  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", "echo hello | cat > /local/pty-cat.txt",
          (char *)NULL);
    _exit(127);
  }
  int status = 0;
  pty_reap(pid, 4000, &status);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

  FILE *f = fopen(path, "r");
  TEST_ASSERT_NOT_NULL(f);
  char line[64];
  TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f));
  TEST_ASSERT_EQUAL_STRING("hello\n", line);
  fclose(f);

  close(master);
  unlink(path);
}

// ---- case 9: argv passed through; PATH lookup; 127 for unknown (M1) ------
void test_pty_argv_path(void) {
  setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/test", 1);
  int status = 0;

  // Absolute path: argv intact.
  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", "/test/test_pty_helper.elf one two three",
          (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  TEST_ASSERT_TRUE(pty_wait_for(master, acc, &accn, "one|two|three", 8000));
  pty_reap(pid, 4000, &status);
  close(master);

  // PATH lookup via the bare name (packaged as /usr/bin/ptytest).
  master = -1;
  pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", "ptytest alpha beta", (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  accn = 0;
  acc[0] = '\0';
  TEST_ASSERT_TRUE(pty_wait_for(master, acc, &accn, "alpha|beta", 8000));
  status = 0;
  pty_reap(pid, 4000, &status);
  close(master);

  // Unknown command: execvp fails, shell reports 127.
  master = -1;
  pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", "definitely-no-such-cmd-xyz", (char *)NULL);
    _exit(127);
  }
  status = 0;
  waitpid(pid, &status, 0);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(127, WEXITSTATUS(status));
  close(master);
}

// ---- case 11: TIOCSWINSZ -> SIGWINCH to the foreground pgid (M1) ---------
void test_pty_resize_winch(void) {
  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/test/test_pty_helper.elf", "test_pty_helper.elf", "--winch",
          (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  // Helper installed the handler before reporting ready.
  int ready_ok = pty_wait_for(master, acc, &accn, "WINCH_READY", 8000);

  struct winsize ws = {40, 80, 0, 0};
  int ws_ok = ready_ok && ioctl(master, TIOCSWINSZ, &ws) == 0;
  int winch_ok = ws_ok && pty_wait_for(master, acc, &accn, "WINCH", 8000);

  close(master);
  int status = 0;
  pty_reap(pid, 4000, &status);

  TEST_ASSERT_TRUE(ready_ok);
  TEST_ASSERT_TRUE(ws_ok);
  TEST_ASSERT_TRUE(winch_ok);
}

// ---- case 12: master close SIGHUPs the foreground pgid; tree reaped ------
void test_pty_master_close_hup(void) {
  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", "/test/test_pty_helper.elf --loop",
          (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  int ready_ok = pty_wait_for(master, acc, &accn, "READY", 8000);

  // Master close -> SIGHUP to the job pgid. The shell (own group) is not
  // signalled; it reaps the job and exits with 128+SIGHUP.
  int close_ok = 0;
  if (ready_ok) {
    close(master);
    close_ok = 1;
  }
  int status = 0;
  pty_reap(pid, 8000, &status);

  TEST_ASSERT_TRUE(ready_ok);
  TEST_ASSERT_TRUE(close_ok);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(129, WEXITSTATUS(status));
}

// ---- M2-A: background slave read -> SIGTTIN, fg+CONT resumes the read ------
void test_pty_bg_read_sigttin(void) {
  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/test/test_pty_helper.elf", "h", "--bgread", (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  // BGSTOPPED:21 = the bg child stopped on SIGTTIN (21).
  int stop_ok = pty_wait_for(master, acc, &accn, "BGSTOPPED:21", 8000);
  // The slave is canonical: submit a line so the resumed read can return.
  int write_ok = stop_ok && write(master, "Z\r", 2) == 2;
  int read_ok = write_ok && pty_wait_for(master, acc, &accn, "BGREAD:Z", 8000);
  int exit_ok = read_ok && pty_wait_for(master, acc, &accn, "BGEXIT:0", 8000);
  close(master);
  int status = 0;
  pty_reap(pid, 4000, &status);
  TEST_ASSERT_TRUE(stop_ok);
  TEST_ASSERT_TRUE(write_ok);
  TEST_ASSERT_TRUE(read_ok);
  TEST_ASSERT_TRUE(exit_ok);
}

// ---- M2-A: background slave write — allowed by default, SIGTTOU on TOSTOP --
void test_pty_bg_write_tostop(void) {
  // Without TOSTOP: a background write succeeds and the child exits 0.
  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/test/test_pty_helper.elf", "h", "--bgwrite", (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  int w_ok = pty_wait_for(master, acc, &accn, "BGWROTE", 8000);
  int e_ok = pty_wait_for(master, acc, &accn, "BGWEXIT:0", 8000);
  close(master);
  int s = 0;
  pty_reap(pid, 4000, &s);
  TEST_ASSERT_TRUE(w_ok);
  TEST_ASSERT_TRUE(e_ok);

  // With TOSTOP: the background write is gated -> SIGTTOU (22) -> stop.
  pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/test/test_pty_helper.elf", "h", "--bgwrite", "tostop",
          (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  accn = 0;
  acc[0] = '\0';
  int sw_ok = pty_wait_for(master, acc, &accn, "BGSTOPPED_W:22", 8000);
  close(master);
  s = 0;
  pty_reap(pid, 4000, &s);
  TEST_ASSERT_TRUE(sw_ok);
}

// ---- M2-A: tcsetpgrp errno matrix (non-ctty / bogus pgid / cross-session) -
void test_pty_tcsetpgrp_errnos(void) {
  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    execl("/test/test_pty_helper.elf", "h", "--ttioerrs", (char *)NULL);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_NONBLOCK);
  char acc[ACC_CAP];
  int accn = 0;
  acc[0] = '\0';
  // R1:-1:25 (ENOTTY)  R2:-1:1 (EPERM)  R3:-1:25 (ENOTTY)
  int r1 = pty_wait_for(master, acc, &accn, "R1:-1:25", 8000);
  int r2 = r1 && pty_wait_for(master, acc, &accn, "R2:-1:1", 8000);
  int r3 = r2 && pty_wait_for(master, acc, &accn, "R3:-1:25", 8000);
  close(master);
  int s = 0;
  pty_reap(pid, 4000, &s);
  TEST_ASSERT_TRUE(r1);
  TEST_ASSERT_TRUE(r2);
  TEST_ASSERT_TRUE(r3);
}

// ---- regression: all shared poll waiters on a PTY must be woken ------------
void test_pty_poll_wakes_all_waiters(void) {
  int master, slave;
  int ready[2], done[2];
  TEST_ASSERT_EQUAL_INT(0, openpty(&master, &slave, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(0, pipe(ready));
  TEST_ASSERT_EQUAL_INT(0, pipe(done));

  pid_t children[2] = {-1, -1};
  for (int i = 0; i < 2; i++) {
    children[i] = fork();
    TEST_ASSERT_TRUE(children[i] >= 0);
    if (children[i] == 0) {
      close(slave);
      close(ready[0]);
      close(done[0]);
      char byte = 'R';
      if (write(ready[1], &byte, 1) != 1)
        _exit(2);

      struct pollfd pfd = {.fd = master, .events = POLLIN, .revents = 0};
      int ret = poll(&pfd, 1, 2000);
      byte = (ret == 1 && (pfd.revents & POLLIN)) ? '1' : '0';
      (void)write(done[1], &byte, 1);
      _exit(byte == '1' ? 0 : 3);
    }
  }

  close(ready[1]);
  close(done[1]);

  int ready_count = 0;
  while (ready_count < 2) {
    char buf[2];
    ssize_t n = read(ready[0], buf, sizeof(buf));
    if (n <= 0)
      break;
    ready_count += (int)n;
  }

  // Let both children enter poll before creating the readiness transition.
  msleep(100);
  int write_ok = write(slave, "X", 1) == 1;

  int flags = fcntl(done[0], F_GETFL, 0);
  if (flags >= 0)
    fcntl(done[0], F_SETFL, flags | O_NONBLOCK);
  int done_count = 0;
  int good_count = 0;
  for (int waited = 0; waited < 500 && done_count < 2; waited += 20) {
    char buf[2];
    ssize_t n = read(done[0], buf, sizeof(buf));
    if (n > 0) {
      done_count += (int)n;
      for (ssize_t i = 0; i < n; i++)
        good_count += buf[i] == '1';
      continue;
    }
    msleep(20);
  }

  close(ready[0]);
  close(done[0]);
  close(master);
  close(slave);
  for (int i = 0; i < 2; i++) {
    kill(children[i], SIGKILL);
    int status = 0;
    pty_reap(children[i], 1000, &status);
  }

  TEST_ASSERT_EQUAL_INT(2, ready_count);
  TEST_ASSERT_TRUE(write_ok);
  TEST_ASSERT_EQUAL_INT(2, done_count);
  TEST_ASSERT_EQUAL_INT(2, good_count);
}

int main(void) {
  alarm(90); // backstop: a stuck kernel/shell path must not hang the runner
  UNITY_BEGIN();
  RUN_TEST(test_pty_openpty_names);
  RUN_TEST(test_pty_forkpty_roundtrip);
  RUN_TEST(test_pty_16_create_destroy);
  RUN_TEST(test_pty_env_inherit);
  RUN_TEST(test_pty_ldisc_erase);
  RUN_TEST(test_pty_ldisc_extended_editing);
  RUN_TEST(test_pty_ctrl_d_eof);
  RUN_TEST(test_pty_raw_mode);
  RUN_TEST(test_pty_ctrl_c_sigint);
  RUN_TEST(test_pty_ctrl_c_shell_survives);
  RUN_TEST(test_pty_pipeline_ctrl_c);
  RUN_TEST(test_pty_pipe_redirect);
  RUN_TEST(test_pty_argv_path);
  RUN_TEST(test_pty_resize_winch);
  RUN_TEST(test_pty_master_close_hup);
  RUN_TEST(test_pty_bg_read_sigttin);
  RUN_TEST(test_pty_bg_write_tostop);
  RUN_TEST(test_pty_tcsetpgrp_errnos);
  RUN_TEST(test_pty_poll_wakes_all_waiters);
  return UNITY_END();
}
