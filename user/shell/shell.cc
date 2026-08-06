/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "linenoise.h"

#include "shell_command.h"

// Read one canonical line when linenoise is unavailable (stdin is not a tty, or
// linenoiseEditStart fails). Mirrors the pre-linenoise behavior: block on stdin
// via the M2-B self-pipe poll, strip the trailing newline. Returns the byte
// count, or -1 on EOF / read error.
static int readline_plain(char *buf, int len) {
  for (;;) {
    shell_wait_input();
    ssize_t n = read(0, buf, (size_t)(len - 1));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EIO && tcsetpgrp(0, getpgrp()) == 0)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      buf[--n] = '\0';
    return (int)n;
  }
}

// ===================== linenoise completion =====================

// Add `candidate` to the completion list unless it is already present
// (linenoise dedups only by exact string in some versions) or the cap is
// reached. Bounded so a broad prefix match cannot flood the terminal.
#define COMPLETION_MAX 64
static void add_completion(linenoiseCompletions *lc, const char *candidate) {
  if (lc->len >= COMPLETION_MAX)
    return;
  for (size_t i = 0; i < lc->len; i++)
    if (strcmp(lc->cvec[i], candidate) == 0)
      return;
  linenoiseAddCompletion(lc, candidate);
}

// Complete a word that contains '/': treat it as a path prefix and list the
// matching entries in its directory. linenoise replaces the whole word with the
// candidate, so the candidate is the full `<dir>/` + matched entry name.
static void complete_path(const char *word, linenoiseCompletions *lc) {
  const char *base = strrchr(word, '/'); // points at the last '/'
  const char *name = base + 1;           // first byte of the to-complete token
  size_t dirlen = (size_t)(name - word); // includes the trailing '/'
  size_t nlen = strlen(name);
  char dir[256];
  if (dirlen == 1) {
    dir[0] = '/';
    dir[1] = '\0';
  } else {
    if (dirlen - 1 >= sizeof(dir))
      return;
    memcpy(dir, word, dirlen - 1);
    dir[dirlen - 1] = '\0';
  }
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' && (nlen == 0 || name[0] != '.'))
      continue; // skip dotfiles unless the prefix starts with '.'
    if (nlen && strncmp(e->d_name, name, nlen) != 0)
      continue;
    char full[512];
    int n =
        snprintf(full, sizeof(full), "%.*s%s", (int)dirlen, word, e->d_name);
    if (n < 0 || (size_t)n >= sizeof(full))
      continue;
    add_completion(lc, full);
  }
  closedir(d);
}

// Complete a bare command name (no '/') by scanning every directory in $PATH
// for executable regular files whose name starts with the prefix.
static void complete_command(const char *word, linenoiseCompletions *lc) {
  size_t wlen = strlen(word);
  const char *path = getenv("PATH");
  if (!path || !wlen)
    return;
  char pathbuf[1024];
  if (strlen(path) >= sizeof(pathbuf))
    return;
  memcpy(pathbuf, path, strlen(path) + 1);
  for (char *dir = strtok(pathbuf, ":"); dir; dir = strtok(NULL, ":")) {
    if (!*dir)
      continue;
    DIR *d = opendir(dir);
    if (!d)
      continue;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (strncmp(e->d_name, word, wlen) != 0)
        continue;
      char full[512];
      int n = snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
      if (n < 0 || (size_t)n >= sizeof(full))
        continue;
      struct stat st;
      if (stat(full, &st) != 0)
        continue;
      if (!S_ISREG(st.st_mode))
        continue;
      if (access(full, X_OK) != 0)
        continue;
      add_completion(lc, e->d_name);
    }
    closedir(d);
  }
}

static void completion_callback(const char *buf, linenoiseCompletions *lc) {
  // Find the start of the word being completed (last whitespace-delimited
  // token).
  const char *p = buf + strlen(buf);
  while (p > buf && !strchr(" \t", p[-1]))
    p--;
  if (strchr(p, '/'))
    complete_path(p, lc);
  else
    complete_command(p, lc);
}

// ===================== linenoise-driven line read =====================

// Drive linenoise's non-blocking edit API over the existing SIGCHLD self-pipe
// poll so a background child exit can still wake the idle shell. Returns:
//   >=0  line length written into `buf` (NUL-terminated)
//   -1   EOF / Ctrl-D on an empty line / unrecoverable error  → caller exits
//   -2   Ctrl-C at the prompt (line cancelled)                → caller
//   re-prompts
#define READLINE_EOF (-1)
#define READLINE_CTRL_C (-2)

static int shell_readline_linenoise(char *buf, int len) {
  // linenoiseEditStart returns -1 when stdin is not a tty (or raw mode fails);
  // fall back to the plain blocking read path used by non-interactive drivers.
  struct linenoiseState ls;
  if (linenoiseEditStart(&ls, 0, 1, buf, (size_t)len, "> ") == -1)
    return readline_plain(buf, len);

  // O_NONBLOCK on fd0 is required: linenoise reads ESC/bracketed-paste
  // sequences with extra read() calls inside linenoiseEditFeed; a blocking fd
  // would hang the shell if a multi-byte sequence were split across reads.
  // Restored before returning so forked children (which inherit fd flags) see a
  // blocking stdin.
  int saved_flags = fcntl(0, F_GETFL);
  if (saved_flags >= 0)
    fcntl(0, F_SETFL, saved_flags | O_NONBLOCK);

  char *res;
  for (;;) {
    int r = shell_poll_input(); // 1=stdin ready, 0=SIGCHLD wake (drained)
    if (r == 0) {
      // Background child state change: reap + notify, bracketed by Hide/Show so
      // the "[jid]+ Done" notice lands above the in-progress edit line instead
      // of tearing through it.
      linenoiseHide(&ls);
      shell_reap_jobs();
      linenoiseShow(&ls);
      continue;
    }
    res = linenoiseEditFeed(&ls);
    if (res == linenoiseEditMore)
      continue;
    if (res == NULL) {
      linenoiseEditStop(&ls);
      if (saved_flags >= 0)
        fcntl(0, F_SETFL, saved_flags);
      // CTRL_C → EAGAIN (cancel line, re-prompt); CTRL_D on empty / EOF / error
      // → ENOENT or other → treat as EOF.
      return errno == EAGAIN ? READLINE_CTRL_C : READLINE_EOF;
    }
    break;
  }
  linenoiseEditStop(&ls);
  if (saved_flags >= 0)
    fcntl(0, F_SETFL, saved_flags);
  return (int)strlen(res);
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
  // complete test suite inside their newly-created PTY. XOS_AUTOTEST is
  // consumed here so programs launched from this shell cannot accidentally
  // start another automatic suite.
  bool run_autotest = getenv("XOS_AUTOTEST") != nullptr &&
                      getenv("XOS_SKIP_AUTOTEST") == nullptr;
#endif
  unsetenv("XOS_AUTOTEST");
#ifdef TEST
  if (run_autotest) {
#ifdef PERF
    int test_status = shell_run_command("/usr/bin/perf");
#else
    int test_status = shell_run_command("/test/test_runner.elf");
#endif
    printf("shell: test runner finished status=%d, entering interactive mode\n",
           test_status);
  }
#endif

  // Interactive line editing (linenoise) + persistent history. Only set up when
  // stdin is a tty; otherwise the plain read path is used with no history.
  static char history_path[256];
  const char *home = getenv("HOME");
  if (isatty(0)) {
    linenoiseSetCompletionCallback(completion_callback);
    linenoiseHistorySetMaxLen(100);
    if (home) {
      snprintf(history_path, sizeof(history_path), "%s/.linenoise_history",
               home);
      linenoiseHistoryLoad(history_path);
    }
  }

  char line[256];

  while (1) {
    // Drain background state changes before drawing the prompt so a finished
    // job's "[jid]+ Done" notice appears above the prompt.
    shell_reap_jobs();
    int n = shell_readline_linenoise(line, sizeof(line));
    if (n == READLINE_CTRL_C)
      continue; // prompt-level Ctrl-C: discard the line, redraw
    if (n == READLINE_EOF) {
      // EOF (master closed / Ctrl-D on empty line): HUP every job and exit.
      shell_hangup_jobs();
      return 0;
    }
    if (n == 0)
      continue; // empty line

    if (isatty(0) && home) {
      linenoiseHistoryAdd(line);
      linenoiseHistorySave(history_path);
    }

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
