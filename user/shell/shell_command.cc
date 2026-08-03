/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// M2-B: job control. The executor is split into a fixed-capacity job table
// (jobs[]) with a single state-entry point (job_update), an async SIGCHLD
// reaper driven by a nonblocking self-pipe, and bg/fg/jobs builtins. The
// parser grows a TOK_BACKGROUND token and a per-pipeline `background` flag.
// Foreground jobs hand the tty to the job pgid before a unified wait
// (WUNTRACED|WCONTINUED) and reclaim the tty + restore the shell termios on
// every exit path (done or stopped). Job-control ioctl is only issued when
// `job_control` is active (interactive mode); `sh -c` / `sh file` reuse the
// same parser/executor but never touch the tty, and bg/fg/jobs return a
// determined non-zero error.

#include "shell_command.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum token_type {
  TOK_WORD,
  TOK_SEMI,
  TOK_AND,
  TOK_OR,
  TOK_PIPE,
  TOK_BACKGROUND, // M2-B: single '&' — backgrounds the preceding and_or
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
  int connector;   // 0 seq/first, 1 &&, 2 ||
  bool background; // M2-B: trailing '&' applied to this pipeline
};
struct program {
  pipeline pipelines[32];
  int count;
};

// ===================== job table (M2-B) =====================
enum pstate { P_RUNNING, P_STOPPED, P_DONE };
enum jstate { J_RUNNING, J_STOPPED, J_DONE };
#define JOB_MAX 16
#define JOB_STAGES 16
struct job {
  int used;
  int jid;
  pid_t pgid;
  pid_t pids[JOB_STAGES];
  int statuses[JOB_STAGES];
  enum pstate pstates[JOB_STAGES];
  int count;
  bool background;
  bool notified; // background DONE already reported to the user
  struct termios tmodes;
  bool tmodes_valid;
  char command[256];
};
static struct job jobs[JOB_MAX];
static int next_jid = 1;

// `job_control` is set by shell_init_jobcontrol (interactive mode only). When
// false (sh -c / sh file), no tty handoff and bg/fg/jobs return an error.
static bool job_control;
static bool job_control_initialized;

// Shell's own termios + pgrp, captured at init for restore after a fg job.
static struct termios shell_tmodes;
static pid_t shell_pgid;

// SIGCHLD self-pipe: handler writes a wake byte; the main loop poll()s it
// alongside stdin so a background child exit can never be missed. CLOEXEC on
// both ends so a forked child never holds the wake channel alive.
static int sigchld_pipe[2] = {-1, -1};

static int last_status;
static int requested_exit = -1;

int shell_requested_exit(void) { return requested_exit; }

// ===================== string helpers =====================
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

// Build a display string for a job: argv joined by spaces, stages by " | ".
static void build_command_string(pipeline &pl, char *buf, int sz) {
  int n = 0;
  buf[0] = 0;
  for (int i = 0; i < pl.count; i++) {
    command &c = pl.commands[i];
    if (i && !append_text(buf, &n, " | "))
      break;
    for (int k = 0; k < c.argc; k++) {
      if (k && !append_char(buf, &n, ' '))
        break;
      if (!append_text(buf, &n, c.argv[k]))
        break;
    }
    if (n >= sz - 2)
      break;
  }
  buf[n] = 0;
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
      if (p[1] == '&') {
        t.type = TOK_AND;
        p += 2;
      } else {
        t.type = TOK_BACKGROUND; // single '&' — backgrounds the and_or
        p += 1;
      }
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
    pl.background = false;
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
    // Trailing separator: ';'/ '&' / && / || / END.
    if (t[i].type == TOK_BACKGROUND) {
      // `a && b &` would background only the last segment; reject it
      // outright rather than silently mis-backgrounding.
      if (pl.connector == 1 || pl.connector == 2)
        return 2;
      pl.background = true;
      next_connector = 0;
      i++;
      // `& ;` / `& &` / `& &&` are malformed; only ';' or another and_or
      // may follow (or end).
      if (t[i].type == TOK_BACKGROUND)
        return 2;
      if (t[i].type == TOK_END)
        break;
      if (t[i].type == TOK_SEMI) {
        i++;
        if (t[i].type == TOK_END)
          return 2;
      } else if (t[i].type == TOK_AND) {
        next_connector = 1;
        i++;
        if (t[i].type == TOK_END)
          return 2;
      } else if (t[i].type == TOK_OR) {
        next_connector = 2;
        i++;
        if (t[i].type == TOK_END)
          return 2;
      } else {
        return 2;
      }
    } else if (t[i].type == TOK_SEMI) {
      next_connector = 0;
      i++;
      if (t[i].type == TOK_END)
        return 2;
    } else if (t[i].type == TOK_AND) {
      next_connector = 1;
      i++;
      if (t[i].type == TOK_END)
        return 2;
    } else if (t[i].type == TOK_OR) {
      next_connector = 2;
      i++;
      if (t[i].type == TOK_END)
        return 2;
    } else if (t[i].type == TOK_END) {
      break;
    } else {
      return 2;
    }
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
// cd/export/unset/exit + the job-control builtins jobs/bg/fg. Everything else
// may run in a pipeline child.
static bool parent_state_builtin(const char *s) {
  return !strcmp(s, "cd") || !strcmp(s, "export") || !strcmp(s, "unset") ||
         !strcmp(s, "exit") || !strcmp(s, "jobs") || !strcmp(s, "bg") ||
         !strcmp(s, "fg");
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

// ---- job table helpers ----
static struct job *job_alloc(void) {
  for (int i = 0; i < JOB_MAX; i++)
    if (!jobs[i].used) {
      memset(&jobs[i], 0, sizeof(jobs[i]));
      jobs[i].used = 1;
      jobs[i].jid = next_jid++;
      return &jobs[i];
    }
  return NULL;
}

static struct job *job_by_jid(const char *spec) {
  // Accepts "%N" or "N"; NULL/empty/%%/%+ → most recent non-done job (the
  // "current job" approximation: highest jid that is still running/stopped).
  if (!spec || !*spec || !strcmp(spec, "%+") || !strcmp(spec, "%%")) {
    struct job *last = NULL;
    int last_jid = -1;
    for (int i = 0; i < JOB_MAX; i++) {
      struct job *j = &jobs[i];
      if (!j->used)
        continue;
      bool done = true;
      for (int k = 0; k < j->count; k++)
        if (j->pstates[k] != P_DONE)
          done = false;
      if (!done && j->jid > last_jid) {
        last = j;
        last_jid = j->jid;
      }
    }
    return last;
  }
  const char *p = spec;
  if (*p == '%')
    p++;
  if (!*p)
    return NULL;
  char *end;
  long jid = strtol(p, &end, 10);
  if (*end || jid <= 0)
    return NULL;
  for (int i = 0; i < JOB_MAX; i++)
    if (jobs[i].used && jobs[i].jid == jid)
      return &jobs[i];
  return NULL;
}

static enum jstate job_state(struct job *j) {
  bool any_running = false, any_stopped = false;
  for (int k = 0; k < j->count; k++) {
    if (j->pstates[k] == P_RUNNING)
      any_running = true;
    else if (j->pstates[k] == P_STOPPED)
      any_stopped = true;
  }
  if (any_running)
    return J_RUNNING;
  if (any_stopped)
    return J_STOPPED;
  return J_DONE;
}

static const char *job_state_str(struct job *j) {
  switch (job_state(j)) {
  case J_RUNNING:
    return "Running";
  case J_STOPPED:
    return "Stopped";
  default:
    return "Done";
  }
}

// Single state-entry point: both the synchronous foreground wait and the async
// reaper funnel here. Locates the stage by pid; ignores unknown children
// (a consistency error, not a crash).
static void job_update(pid_t pid, int status) {
  for (int i = 0; i < JOB_MAX; i++) {
    struct job *j = &jobs[i];
    if (!j->used)
      continue;
    for (int k = 0; k < j->count; k++) {
      if (j->pids[k] != pid)
        continue;
      j->statuses[k] = status;
      if (WIFSTOPPED(status))
        j->pstates[k] = P_STOPPED;
      else if (WIFCONTINUED(status))
        j->pstates[k] = P_RUNNING;
      else if (WIFEXITED(status) || WIFSIGNALED(status))
        j->pstates[k] = P_DONE;
      return;
    }
  }
}

// Print completion notices for background jobs that finished, then release
// their slots. Foreground Done jobs are freed by their launch path.
static void job_notify_done(void) {
  for (int i = 0; i < JOB_MAX; i++) {
    struct job *j = &jobs[i];
    if (!j->used || !j->background)
      continue;
    if (job_state(j) == J_DONE && !j->notified) {
      printf("[%d]+ %s\t\t%s\n", j->jid, job_state_str(j), j->command);
      fflush(stdout);
      j->notified = 1;
    }
    if (job_state(j) == J_DONE && j->notified) {
      // Free only after the notice has been shown.
      j->used = 0;
    }
  }
}

static void sigchld_handler(int sig) {
  (void)sig;
  // write() is async-signal-safe. EAGAIN means the pipe is already full of
  // wake bytes — the persistent readable state is enough to wake poll.
  char c = 1;
  (void)write(sigchld_pipe[1], &c, 1);
}

// Drain the self-pipe wake bytes.
void shell_drain_sigchld(void) {
  if (sigchld_pipe[0] < 0)
    return;
  char tmp[64];
  while (read(sigchld_pipe[0], tmp, sizeof(tmp)) > 0)
    ;
}

// Non-blocking reap: drain every reportable child state change into the job
// table and notify on background completions. Called from the main loop after
// poll wakes, before the prompt, and after each foreground wait.
void shell_reap_jobs(void) {
  // A synchronous foreground wait consumes the child status before the main
  // loop observes the SIGCHLD self-pipe. Keep the notification pipe in sync
  // with the wait queue so a stale wake byte cannot poison the next input
  // wait.
  shell_drain_sigchld();
  for (;;) {
    int status = 0;
    pid_t w = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
    if (w > 0)
      job_update(w, status);
    else
      break;
  }
  job_notify_done();
}

// Resume a stopped job into the background.
static int builtin_bg(struct job *j) {
  if (!j) {
    fprintf(stderr, "bg: no such job\n");
    return 1;
  }
  if (kill(-j->pgid, SIGCONT) < 0) {
    fprintf(stderr, "bg: %s\n", strerror(errno));
    return 1;
  }
  j->background = true;
  for (int k = 0; k < j->count; k++)
    if (j->pstates[k] == P_STOPPED)
      j->pstates[k] = P_RUNNING;
  printf("[%d]+ %s\t\t%s\n", j->jid, job_state_str(j), j->command);
  fflush(stdout);
  return 0;
}

// Bring a job to the foreground: hand it the tty, restore its termios, SIGCONT
// if it was stopped, then synchronously wait.
static int builtin_fg(struct job *j) {
  if (!j) {
    fprintf(stderr, "fg: no such job\n");
    return 1;
  }
  if (job_state(j) == J_DONE) {
    fprintf(stderr, "fg: job has terminated\n");
    return 1;
  }
  j->background = false;
  if (job_control) {
    tcsetpgrp(0, j->pgid);
    if (j->tmodes_valid)
      tcsetattr(0, TCSANOW, &j->tmodes);
  }
  bool was_stopped = (job_state(j) == J_STOPPED);
  if (was_stopped && kill(-j->pgid, SIGCONT) < 0) {
    fprintf(stderr, "fg: %s\n", strerror(errno));
  }
  // Synchronous foreground wait.
  int result = 0;
  for (;;) {
    int status = 0;
    pid_t w = waitpid(-1, &status, WUNTRACED | WCONTINUED);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    job_update(w, status);
    shell_reap_jobs(); // reap concurrent background exits
    enum jstate s = job_state(j);
    if (s == J_DONE || s == J_STOPPED)
      break;
  }
  if (job_control) {
    // Save the (possibly modified) termios before restoring ours.
    if (job_state(j) == J_STOPPED) {
      tcgetattr(0, &j->tmodes);
      j->tmodes_valid = true;
    }
    tcsetattr(0, TCSANOW, &shell_tmodes);
    tcsetpgrp(0, shell_pgid);
  }
  // Pipeline status = last stage.
  int st = j->statuses[j->count - 1];
  if (WIFEXITED(st))
    result = WEXITSTATUS(st);
  else if (WIFSIGNALED(st))
    result = 128 + WTERMSIG(st);
  if (job_state(j) == J_DONE)
    j->used = 0; // foreground done: free immediately
  return result;
}

// HUP cleanup on shell exit: SIGHUP then SIGCONT every running/stopped job so
// a stopped job gets to handle SIGHUP, then a bounded non-blocking drain.
void shell_hangup_jobs(void) {
  for (int i = 0; i < JOB_MAX; i++) {
    struct job *j = &jobs[i];
    if (!j->used)
      continue;
    enum jstate s = job_state(j);
    if (s == J_DONE)
      continue;
    kill(-j->pgid, SIGHUP);
    kill(-j->pgid, SIGCONT);
  }
  for (int round = 0; round < 50; round++) {
    shell_reap_jobs();
    bool any = false;
    for (int i = 0; i < JOB_MAX; i++)
      if (jobs[i].used && job_state(&jobs[i]) != J_DONE) {
        any = true;
        break;
      }
    if (!any)
      break;
    struct timespec ts = {0, 20000000}; // 20ms
    nanosleep(&ts, NULL);
  }
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
  if (!strcmp(s, "jobs")) {
    shell_reap_jobs();
    for (int i = 0; i < JOB_MAX; i++) {
      struct job *j = &jobs[i];
      if (!j->used)
        continue;
      printf("[%d]%c %s\t\t%s\n", j->jid, j->background ? '-' : '+',
             job_state_str(j), j->command);
    }
    fflush(stdout);
    return 0;
  }
  if (!strcmp(s, "bg")) {
    if (!job_control) {
      fprintf(stderr, "bg: no job control\n");
      return 1;
    }
    return builtin_bg(job_by_jid(cmd.argc > 1 ? cmd.argv[1] : NULL));
  }
  if (!strcmp(s, "fg")) {
    if (!job_control) {
      fprintf(stderr, "fg: no job control\n");
      return 1;
    }
    return builtin_fg(job_by_jid(cmd.argc > 1 ? cmd.argv[1] : NULL));
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
  // A forked child must not hold the shell's SIGCHLD wake pipe open.
  if (sigchld_pipe[0] >= 0) {
    close(sigchld_pipe[0]);
    close(sigchld_pipe[1]);
    sigchld_pipe[0] = sigchld_pipe[1] = -1;
  }
}

// Interactive-shell setup: own process group, ignore terminal signals, take the
// controlling terminal as foreground, install the SIGCHLD self-pipe. Called
// once from main; idempotent.
void shell_init_jobcontrol(void) {
  if (job_control_initialized)
    return;
  job_control_initialized = true;
  job_control = true;

  shell_pgid = getpgrp();
  tcgetattr(0, &shell_tmodes);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
  sigaction(SIGTSTP, &sa, NULL);
  sigaction(SIGTTIN, &sa, NULL);
  sigaction(SIGTTOU, &sa, NULL);

  // SIGCHLD self-pipe + handler. SA_RESTART so foreground waitpid / read are
  // not spuriously interrupted; wake-up is via the persistent pipe-readable
  // state, not EINTR. SA_NOCLDSTOP is NOT set — we want stop/continue reports.
  if (pipe(sigchld_pipe) == 0) {
    fcntl(sigchld_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(sigchld_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(sigchld_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(sigchld_pipe[1], F_SETFD, FD_CLOEXEC);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
  }
  // setpgid/tcsetpgrp idempotent: forkpty's login_tty already made us session
  // leader + foreground. After setsid, sid==pgid==pid so setpgid(0,0) is a
  // no-op (and the kernel returns EPERM for a session leader — ignored).
  setpgid(0, 0);
  tcsetpgrp(0, getpgrp());
}

// Wait for a foreground job to reach DONE or STOPPED. Reaps concurrent
// background exits through the same single state-entry point.
static int job_wait_foreground(struct job *j) {
  for (;;) {
    int status = 0;
    pid_t w = waitpid(-1, &status, WUNTRACED | WCONTINUED);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      break; // ECHILD: nothing left
    }
    job_update(w, status);
    shell_reap_jobs();
    enum jstate s = job_state(j);
    if (s == J_DONE || s == J_STOPPED)
      break;
  }
  // tty restore (safe: shell ignores SIGTTOU; tcsetpgrp from foreground).
  if (job_control) {
    if (job_state(j) == J_STOPPED) {
      tcgetattr(0, &j->tmodes);
      j->tmodes_valid = true;
    }
    tcsetattr(0, TCSANOW, &shell_tmodes);
    tcsetpgrp(0, shell_pgid);
  }
  int result = 0;
  int st = j->statuses[j->count - 1];
  if (WIFEXITED(st))
    result = WEXITSTATUS(st);
  else if (WIFSIGNALED(st))
    result = 128 + WTERMSIG(st);
  return result;
}

static int job_launch(pipeline &pl, bool foreground) {
  // Parent-state builtins run in the parent (foreground, single command).
  // Backgrounding or piping them is rejected — a forked copy cannot change
  // the real shell's state.
  if (pl.count == 1 &&
      (!pl.commands[0].argc || parent_state_builtin(pl.commands[0].argv[0]))) {
    if (!foreground) {
      fprintf(stderr, "sh: cannot background a builtin\n");
      return 1;
    }
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
  // Reject a parent-state builtin appearing anywhere in a pipeline.
  for (int i = 0; i < pl.count; i++) {
    if (pl.commands[i].argc && parent_state_builtin(pl.commands[i].argv[0])) {
      fprintf(stderr, "sh: %s: cannot use in a pipeline\n",
              pl.commands[i].argv[0]);
      return 1;
    }
  }

  struct job *j = job_alloc();
  if (!j) {
    fprintf(stderr, "sh: too many jobs\n");
    return 1;
  }
  build_command_string(pl, j->command, (int)sizeof(j->command));
  j->background = !foreground;
  j->count = pl.count;

  pid_t job_pgid = 0;
  int previous = -1;
  for (int i = 0; i < pl.count; i++) {
    int fds[2] = {-1, -1};
    if (i + 1 < pl.count && pipe(fds) < 0) {
      // Mid-launch failure: terminate + reap already-started stages.
      j->used = 0;
      return 1;
    }
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
      setpgid(0, i == 0 ? 0 : job_pgid);
      reset_signal_defaults();
      apply_assignments(cmd);
      if (apply_redirects(cmd) < 0)
        _exit(1);
      if (is_builtin(cmd)) {
        int st = run_builtin(cmd);
        fflush(0);
        _exit(st);
      }
      execvp(cmd.argv[0], cmd.argv);
      fprintf(stderr, "sh: %s: %s\n", cmd.argv[0],
              errno == ENOENT ? "command not found" : "permission denied");
      _exit(errno == ENOENT ? 127 : 126);
    }
    if (pid < 0) {
      if (previous >= 0)
        close(previous);
      if (fds[0] >= 0)
        close(fds[0]);
      if (fds[1] >= 0)
        close(fds[1]);
      j->used = 0;
      return 1;
    }
    j->pids[i] = pid;
    j->pstates[i] = P_RUNNING;
    if (i == 0) {
      job_pgid = pid;
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

  j->pgid = job_pgid;

  if (!foreground) {
    // Background launch: do not touch the tty; report and return.
    printf("[%d] %d\n", j->jid, (int)job_pgid);
    fflush(stdout);
    return 0;
  }

  // Foreground: hand the terminal to the job, wait, reclaim.
  if (job_control)
    tcsetpgrp(0, job_pgid);
  int result = job_wait_foreground(j);
  if (job_state(j) == J_DONE)
    j->used = 0; // foreground done + reported: free the slot
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
    status = job_launch(pl, !pl.background);
    last_status = status;
    // A backgrounded pipeline yields status 0 to the && / || chain and to the
    // next pipeline; its real exit is reported asynchronously by the reaper.
    if (pl.background)
      status = 0;
    if (requested_exit >= 0)
      return requested_exit;
    if (job_control_initialized)
      shell_reap_jobs();
  }
  return status;
}

// Block until stdin is readable or a SIGCHLD wake byte arrives. Used by the
// interactive main loop so a background child exit can never leave the shell
// stuck in a blocking read. Returns 1 = stdin ready, 0 = wake only.
int shell_wait_input(void) {
  struct pollfd pfd[2];
  int n = 0;
  pfd[n].fd = 0;
  pfd[n].events = POLLIN;
  n++;
  if (sigchld_pipe[0] >= 0) {
    pfd[n].fd = sigchld_pipe[0];
    pfd[n].events = POLLIN;
    n++;
  }
  for (;;) {
    int r = poll(pfd, (nfds_t)n, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return 1; // fall back to a direct read
    }
    if (n > 1 && (pfd[1].revents & POLLIN)) {
      shell_drain_sigchld();
      shell_reap_jobs();
      // If stdin is also ready, fall through to read it.
      if (pfd[0].revents & POLLIN)
        return 1;
      continue; // wake-only: re-poll for input
    }
    if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))
      return 1;
  }
}
