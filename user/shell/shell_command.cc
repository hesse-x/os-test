/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "shell_command.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum token_type {
  TOK_WORD,
  TOK_SEMI,
  TOK_AND,
  TOK_OR,
  TOK_PIPE,
  TOK_IN,
  TOK_OUT,
  TOK_APPEND,
  TOK_ERR,
  TOK_ERR_TO_OUT,
  TOK_END
};

struct token {
  token_type type;
  char text[256];
};
struct redir {
  token_type type;
  const char *path;
};
struct command {
  char *argv[32];
  int argc;
  char *assign[16];
  int nassign;
  redir redirects[16];
  int nredirect;
};
struct pipeline {
  command commands[16];
  int count;
  int connector;
};
struct program {
  pipeline pipelines[32];
  int count;
};

static int last_status;
static int requested_exit = -1;

// Queried by the interactive loop to distinguish `exit` from a command that
// happened to return the same code (step1.md §3.5).
int shell_requested_exit(void) { return requested_exit; }

static bool append_char(char *out, int *n, char c) {
  if (*n >= 255)
    return false;
  out[(*n)++] = c;
  return true;
}

static bool append_text(char *out, int *n, const char *s) {
  while (*s)
    if (!append_char(out, n, *s++))
      return false;
  return true;
}

static const char *expand_var(const char *p, char *out, int *n, bool *ok) {
  if (*p == '?') {
    char number[16];
    snprintf(number, sizeof(number), "%d", last_status);
    *ok = append_text(out, n, number);
    return p + 1;
  }
  char name[128];
  int k = 0;
  if (*p == '{') {
    p++;
    while (*p && *p != '}' && k < 127)
      name[k++] = *p++;
    if (*p != '}') {
      *ok = false;
      return p;
    }
    p++;
  } else {
    if (!(isalpha((unsigned char)*p) || *p == '_')) {
      *ok = append_char(out, n, '$');
      return p;
    }
    while ((isalnum((unsigned char)*p) || *p == '_') && k < 127)
      name[k++] = *p++;
  }
  name[k] = 0;
  const char *value = getenv(name);
  *ok = !value || append_text(out, n, value);
  return p;
}

static int lex(const char *p, token *tokens, int *count) {
  int nt = 0;
  while (*p) {
    while (isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;
    if (nt >= 127)
      return 2;
    token &t = tokens[nt];
    t.text[0] = 0;
    if (*p == ';') {
      t.type = TOK_SEMI;
      p++;
      nt++;
      continue;
    }
    if (*p == '&') {
      if (p[1] != '&')
        return 2;
      t.type = TOK_AND;
      p += 2;
      nt++;
      continue;
    }
    if (*p == '|') {
      t.type = p[1] == '|' ? TOK_OR : TOK_PIPE;
      p += p[1] == '|' ? 2 : 1;
      nt++;
      continue;
    }
    if (*p == '<') {
      if (p[1] == '<')
        return 2;
      t.type = TOK_IN;
      p++;
      nt++;
      continue;
    }
    if (*p == '>') {
      t.type = p[1] == '>' ? TOK_APPEND : TOK_OUT;
      p += p[1] == '>' ? 2 : 1;
      nt++;
      continue;
    }
    if (p[0] == '2' && p[1] == '>') {
      if (p[2] == '&' && p[3] == '1') {
        t.type = TOK_ERR_TO_OUT;
        p += 4;
      } else {
        t.type = TOK_ERR;
        p += 2;
      }
      nt++;
      continue;
    }
    if (*p == '(' || *p == ')' || *p == '`' || *p == '*' || *p == '?' ||
        *p == '[' || *p == ']')
      return 2;

    t.type = TOK_WORD;
    int n = 0;
    bool started = false;
    while (*p && !isspace((unsigned char)*p) && !strchr(";&|<>", *p)) {
      started = true;
      if (*p == '\\') {
        p++;
        if (!*p || !append_char(t.text, &n, *p++))
          return 2;
      } else if (*p == '\'') {
        p++;
        while (*p && *p != '\'')
          if (!append_char(t.text, &n, *p++))
            return 2;
        if (*p != '\'')
          return 2;
        p++;
      } else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
          if (*p == '\\') {
            p++;
            if (!*p || !append_char(t.text, &n, *p++))
              return 2;
          } else if (*p == '$') {
            if (p[1] == '(')
              return 2;
            bool ok = true;
            p = expand_var(p + 1, t.text, &n, &ok);
            if (!ok)
              return 2;
          } else if (!append_char(t.text, &n, *p++))
            return 2;
        }
        if (*p != '"')
          return 2;
        p++;
      } else if (*p == '$') {
        if (p[1] == '(')
          return 2;
        bool ok = true;
        p = expand_var(p + 1, t.text, &n, &ok);
        if (!ok)
          return 2;
      } else {
        if (*p == '`' || *p == '*' || *p == '?' || *p == '[' || *p == ']')
          return 2;
        if (!append_char(t.text, &n, *p++))
          return 2;
      }
    }
    if (!started)
      return 2;
    t.text[n] = 0;
    nt++;
  }
  tokens[nt++].type = TOK_END;
  *count = nt;
  return 0;
}

static bool assignment_word(const char *s) {
  const char *eq = strchr(s, '=');
  if (!eq || eq == s || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
    return false;
  for (const char *p = s + 1; p < eq; p++)
    if (!(isalnum((unsigned char)*p) || *p == '_'))
      return false;
  return true;
}

static int parse(token *t, program *out) {
  memset(out, 0, sizeof(*out));
  int i = 0, next_connector = 0;
  while (t[i].type != TOK_END) {
    if (out->count >= 32)
      return 2;
    pipeline &pl = out->pipelines[out->count++];
    pl.connector = next_connector;
    for (;;) {
      if (pl.count >= 16)
        return 2;
      command &cmd = pl.commands[pl.count++];
      memset(&cmd, 0, sizeof(cmd));
      bool can_assign = true;
      while (t[i].type == TOK_WORD || t[i].type == TOK_IN ||
             t[i].type == TOK_OUT || t[i].type == TOK_APPEND ||
             t[i].type == TOK_ERR || t[i].type == TOK_ERR_TO_OUT) {
        if (t[i].type == TOK_WORD) {
          if (can_assign && assignment_word(t[i].text)) {
            if (cmd.nassign >= 16)
              return 2;
            cmd.assign[cmd.nassign++] = t[i].text;
          } else {
            can_assign = false;
            if (cmd.argc >= 31)
              return 2;
            cmd.argv[cmd.argc++] = t[i].text;
          }
          i++;
        } else {
          if (cmd.nredirect >= 16)
            return 2;
          redir &r = cmd.redirects[cmd.nredirect++];
          r.type = t[i++].type;
          if (r.type != TOK_ERR_TO_OUT) {
            if (t[i].type != TOK_WORD)
              return 2;
            r.path = t[i++].text;
          }
        }
      }
      cmd.argv[cmd.argc] = 0;
      if (!cmd.argc && !cmd.nassign)
        return 2;
      if (t[i].type != TOK_PIPE)
        break;
      i++;
      if (t[i].type == TOK_END)
        return 2;
    }
    if (t[i].type == TOK_SEMI)
      next_connector = 0;
    else if (t[i].type == TOK_AND)
      next_connector = 1;
    else if (t[i].type == TOK_OR)
      next_connector = 2;
    else if (t[i].type == TOK_END)
      break;
    else
      return 2;
    i++;
    if (t[i].type == TOK_END)
      return 2;
  }
  return out->count ? 0 : 2;
}

static void apply_assignments(command &cmd) {
  for (int i = 0; i < cmd.nassign; i++) {
    char *eq = strchr(cmd.assign[i], '=');
    *eq = 0;
    setenv(cmd.assign[i], eq + 1, 1);
    *eq = '=';
  }
}

static int apply_redirects(command &cmd) {
  for (int i = 0; i < cmd.nredirect; i++) {
    redir &r = cmd.redirects[i];
    if (r.type == TOK_ERR_TO_OUT) {
      if (dup2(1, 2) < 0)
        return -1;
      continue;
    }
    int target = r.type == TOK_IN ? 0 : (r.type == TOK_ERR ? 2 : 1);
    int flags = r.type == TOK_IN ? O_RDONLY : O_WRONLY | O_CREAT;
    if (r.type == TOK_APPEND)
      flags |= O_APPEND;
    else if (r.type != TOK_IN)
      flags |= O_TRUNC;
    int fd = open(r.path, flags, 0666);
    if (fd < 0 || dup2(fd, target) < 0) {
      if (fd >= 0)
        close(fd);
      return -1;
    }
    close(fd);
  }
  return 0;
}

// Builtins that must run in the parent process (they change shell state):
// cd/export/unset/exit. Everything else may run in a pipeline child.
static bool parent_state_builtin(const char *s) {
  return !strcmp(s, "cd") || !strcmp(s, "export") || !strcmp(s, "unset") ||
         !strcmp(s, "exit");
}

static bool is_builtin(command &cmd) {
  if (!cmd.argc)
    return true;
  const char *s = cmd.argv[0];
  return parent_state_builtin(s) || !strcmp(s, "pwd") || !strcmp(s, "echo") ||
         !strcmp(s, "true") || !strcmp(s, "false") || !strcmp(s, "ls") ||
         !strcmp(s, "cat") || !strcmp(s, "touch") || !strcmp(s, "mkdir") ||
         !strcmp(s, "clear");
}

// These file/stat builtins resolve relative paths against the kernel cwd
// (the shell keeps the real cwd via chdir), so no private cwd bookkeeping is
// needed — unlike the pre-unification interactive shell.
static int run_ls(const char *path, int long_format) {
  char base[512];
  if (!path || !*path) {
    if (!getcwd(base, sizeof(base)))
      return 1;
    path = base;
  }
  DIR *dir = opendir(path);
  if (!dir) {
    fprintf(stderr, "ls: cannot access %s\n", path);
    return 1;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    if (!long_format) {
      printf("%s\n", entry->d_name);
      continue;
    }
    char full[768];
    snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
    struct stat st;
    if (stat(full, &st) == 0) {
      printf("%s %d root root %u %s\n",
             S_ISDIR(st.st_mode) ? "drwxr-xr-x" : "-rw-r--r--",
             S_ISDIR(st.st_mode) ? 2 : 1, (unsigned)st.st_size, entry->d_name);
    } else {
      printf("?????????? 1 root root 0 %s\n", entry->d_name);
    }
  }
  closedir(dir);
  return 0;
}

static int run_cat(const char *path) {
  int fd = 0; /* no path: copy stdin to stdout (e.g. `echo x | cat`) */
  if (path) {
    fd = open(path, O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "cat: cannot open %s\n", path);
      return 1;
    }
  }
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    for (ssize_t i = 0; i < n; i++)
      putchar(buf[i]);
  if (fd != 0)
    close(fd);
  return 0;
}

static int run_touch(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT, 0666);
  if (fd < 0) {
    fprintf(stderr, "touch: %s: %s\n", path, strerror(errno));
    return 1;
  }
  close(fd);
  return 0;
}

static int run_mkdir(const char *path) {
  if (mkdir(path, 0755) != 0) {
    fprintf(stderr, "mkdir: %s: %s\n", path, strerror(errno));
    return 1;
  }
  return 0;
}

static int run_builtin(command &cmd) {
  apply_assignments(cmd);
  if (!cmd.argc)
    return 0;
  const char *s = cmd.argv[0];
  if (!strcmp(s, "true"))
    return 0;
  if (!strcmp(s, "false"))
    return 1;
  if (!strcmp(s, "echo")) {
    for (int i = 1; i < cmd.argc; i++)
      printf("%s%s", i == 1 ? "" : " ", cmd.argv[i]);
    putchar('\n');
    return 0;
  }
  if (!strcmp(s, "pwd")) {
    char path[512];
    if (!getcwd(path, sizeof(path)))
      return 1;
    puts(path);
    return 0;
  }
  if (!strcmp(s, "cd")) {
    const char *path = cmd.argc > 1 ? cmd.argv[1] : getenv("HOME");
    if (!path)
      path = "/";
    if (chdir(path) < 0) {
      fprintf(stderr, "sh: cd: %s\n", path);
      return 1;
    }
    return 0;
  }
  if (!strcmp(s, "export")) {
    // export NAME=value  sets the shell environment (parent process).
    for (int i = 1; i < cmd.argc; i++) {
      char *eq = strchr(cmd.argv[i], '=');
      if (!eq || eq == cmd.argv[i])
        continue;
      *eq = 0;
      setenv(cmd.argv[i], eq + 1, 1);
      *eq = '=';
    }
    return 0;
  }
  if (!strcmp(s, "unset")) {
    for (int i = 1; i < cmd.argc; i++)
      unsetenv(cmd.argv[i]);
    return 0;
  }
  if (!strcmp(s, "clear")) {
    printf("\033[2J\033[H");
    return 0;
  }
  if (!strcmp(s, "ls")) {
    int long_fmt = 0;
    const char *path = NULL;
    for (int i = 1; i < cmd.argc; i++) {
      if (!strcmp(cmd.argv[i], "-l"))
        long_fmt = 1;
      else
        path = cmd.argv[i];
    }
    return run_ls(path, long_fmt);
  }
  if (!strcmp(s, "cat"))
    return run_cat(cmd.argc > 1 ? cmd.argv[1] : NULL);
  if (!strcmp(s, "touch"))
    return cmd.argc > 1 ? run_touch(cmd.argv[1]) : 1;
  if (!strcmp(s, "mkdir"))
    return cmd.argc > 1 ? run_mkdir(cmd.argv[1]) : 1;
  if (!strcmp(s, "exit")) {
    int status = cmd.argc > 1 ? atoi(cmd.argv[1]) & 255 : last_status;
    requested_exit = status;
    return status;
  }
  return 0;
}

// Reset terminal signals to default before exec. The shell ignores these for
// itself; POSIX keeps SIG_IGN across exec, so a child that did not reset here
// would survive Ctrl-C (step1.md §3.6).
static void reset_signal_defaults(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
  sigaction(SIGTSTP, &sa, NULL);
  sigaction(SIGTTIN, &sa, NULL);
  sigaction(SIGTTOU, &sa, NULL);
}

// Interactive-shell setup: the shell lives in its own process group, ignores
// terminal-generated signals, and holds the controlling terminal as its
// foreground group. Idempotent (the forkpty child already did setsid +
// TIOCSCTTY via libc login_tty).
void shell_init_jobcontrol(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
  sigaction(SIGTSTP, &sa, NULL);
  sigaction(SIGTTIN, &sa, NULL);
  sigaction(SIGTTOU, &sa, NULL);
  setpgid(0, 0); // own group (idempotent)
  tcsetpgrp(0, getpgrp());
}

static int run_pipeline(pipeline &pl) {
  // Only parent-state builtins (cd/export/unset/exit) and assignment-only
  // commands run in the parent: they must affect the shell, with redirects
  // applied around them via the saved fds. Everything else — including
  // blocking builtins like `cat` — goes through the job-control child path so
  // ^C (routed to the job's pgid) can reach it; running `cat` in the parent
  // would let it ignore SIGINT along with the shell and hang forever.
  if (pl.count == 1 &&
      (!pl.commands[0].argc || parent_state_builtin(pl.commands[0].argv[0]))) {
    int saved[3] = {dup(0), dup(1), dup(2)};
    int status =
        apply_redirects(pl.commands[0]) < 0 ? 1 : run_builtin(pl.commands[0]);
    fflush(0);
    for (int i = 0; i < 3; i++) {
      if (saved[i] >= 0) {
        dup2(saved[i], i);
        close(saved[i]);
      }
    }
    return status;
  }

  // Foreground pipeline: every process joins one new process group (the first
  // child's pid). Both sides call setpgid to close the fork/exec race; the
  // terminal is handed to the job before waiting and reclaimed after
  // (step1.md §3.6).
  pid_t job_pgid = 0;
  int previous = -1;
  pid_t pids[16];
  for (int i = 0; i < pl.count; i++) {
    int fds[2] = {-1, -1};
    if (i + 1 < pl.count && pipe(fds) < 0)
      return 1;
    pid_t pid = fork();
    if (pid == 0) {
      if (previous >= 0)
        dup2(previous, 0);
      if (fds[1] >= 0)
        dup2(fds[1], 1);
      if (previous >= 0)
        close(previous);
      if (fds[0] >= 0)
        close(fds[0]);
      if (fds[1] >= 0)
        close(fds[1]);
      command &cmd = pl.commands[i];
      // The first child cannot know the group id before fork returns, so it
      // starts its own group; later children join the established one.
      setpgid(0, i == 0 ? 0 : job_pgid);
      reset_signal_defaults();
      apply_assignments(cmd);
      if (apply_redirects(cmd) < 0)
        _exit(1);
      if (is_builtin(cmd)) {
        int st = run_builtin(cmd);
        // _exit skips stdio flush: redirected output (stdout = a file) would
        // otherwise stay in the 4 KiB buffer and the file would be empty.
        fflush(0);
        _exit(st);
      }
      execvp(cmd.argv[0], cmd.argv);
      fprintf(stderr, "sh: %s: %s\n", cmd.argv[0],
              errno == ENOENT ? "command not found" : "permission denied");
      _exit(errno == ENOENT ? 127 : 126);
    }
    if (pid < 0)
      return 1;
    pids[i] = pid;
    // Both sides call setpgid to close the fork/exec race (the child does its
    // own above): without this parent-side call, waitpid(-job_pgid) can run
    // before the child's setpgid, find no child in the group, and bail with
    // ECHILD — reporting a bogus status 1 for e.g. `sh -c true`.
    if (i == 0) {
      job_pgid = pid; // first child's pid is the pipeline's group id
      setpgid(pid, pid);
    } else {
      setpgid(pid, job_pgid);
    }
    if (previous >= 0)
      close(previous);
    if (fds[1] >= 0)
      close(fds[1]);
    previous = fds[0];
  }
  if (previous >= 0)
    close(previous);

  // Hand the terminal to the job; the ldisc now routes ^C to the pipeline.
  tcsetpgrp(0, job_pgid);
  int result = 1;
  for (;;) {
    int status = 0;
    pid_t w = waitpid(-job_pgid, &status, WUNTRACED);
    if (w < 0) {
      if (errno == ECHILD)
        break;  // group fully reaped
      continue; // EINTR — retry
    }
    if (w == pids[pl.count - 1]) {
      // Pipeline status = last stage's exit (matching old semantics).
      if (WIFEXITED(status))
        result = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
        result = 128 + WTERMSIG(status);
    }
  }
  // Reclaim the terminal (safe: the shell ignores SIGTTOU).
  tcsetpgrp(0, getpgrp());
  return result;
}

int shell_run_command(const char *source) {
  token tokens[128];
  int count = 0;
  program prog;
  requested_exit = -1;
  if (lex(source, tokens, &count) || parse(tokens, &prog)) {
    fprintf(stderr, "sh: unsupported or invalid syntax\n");
    return 2;
  }
  int status = 0;
  for (int i = 0; i < prog.count; i++) {
    pipeline &pl = prog.pipelines[i];
    if ((pl.connector == 1 && status != 0) ||
        (pl.connector == 2 && status == 0))
      continue;
    status = run_pipeline(pl);
    last_status = status;
    if (requested_exit >= 0)
      return requested_exit;
  }
  return status;
}
