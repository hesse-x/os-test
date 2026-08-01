/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "shell_command.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static bool is_builtin(command &cmd) {
  if (!cmd.argc)
    return true;
  const char *s = cmd.argv[0];
  return !strcmp(s, "cd") || !strcmp(s, "pwd") || !strcmp(s, "echo") ||
         !strcmp(s, "true") || !strcmp(s, "false") || !strcmp(s, "exit");
}

static int run_builtin(command &cmd) {
  apply_assignments(cmd);
  if (!cmd.argc)
    return 0;
  if (!strcmp(cmd.argv[0], "true"))
    return 0;
  if (!strcmp(cmd.argv[0], "false"))
    return 1;
  if (!strcmp(cmd.argv[0], "echo")) {
    for (int i = 1; i < cmd.argc; i++)
      printf("%s%s", i == 1 ? "" : " ", cmd.argv[i]);
    putchar('\n');
    return 0;
  }
  if (!strcmp(cmd.argv[0], "pwd")) {
    char path[512];
    if (!getcwd(path, sizeof(path)))
      return 1;
    puts(path);
    return 0;
  }
  if (!strcmp(cmd.argv[0], "cd")) {
    const char *path = cmd.argc > 1 ? cmd.argv[1] : getenv("HOME");
    if (!path)
      path = "/";
    if (chdir(path) < 0) {
      fprintf(stderr, "sh: cd: %s\n", path);
      return 1;
    }
    return 0;
  }
  int status = cmd.argc > 1 ? atoi(cmd.argv[1]) & 255 : last_status;
  requested_exit = status;
  return status;
}

static int run_pipeline(pipeline &pl) {
  if (pl.count == 1 && is_builtin(pl.commands[0])) {
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
      apply_assignments(cmd);
      if (apply_redirects(cmd) < 0)
        _exit(1);
      if (is_builtin(cmd))
        _exit(run_builtin(cmd));
      execvp(cmd.argv[0], cmd.argv);
      _exit(errno == ENOENT ? 127 : 126);
    }
    if (pid < 0)
      return 1;
    pids[i] = pid;
    if (previous >= 0)
      close(previous);
    if (fds[1] >= 0)
      close(fds[1]);
    previous = fds[0];
  }
  if (previous >= 0)
    close(previous);
  int result = 1;
  for (int i = 0; i < pl.count; i++) {
    int status = 0;
    if (waitpid(pids[i], &status, 0) == pids[i] && i == pl.count - 1) {
      if (WIFEXITED(status))
        result = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
        result = 128 + WTERMSIG(status);
    }
  }
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
