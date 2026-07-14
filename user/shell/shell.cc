/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <xos/errno.h>

// Current working directory
static char cwd[256] = "/";

static char getc() {
  char ch;
  while (read(0, &ch, 1) != 1) {
  }
  return ch;
}

static int readline(char *buf, int len) {
  struct termios t;
  tcgetattr(0, &t);
  int do_echo =
      !(t.c_lflag & ECHO); // echo only if Terminal ldisc is NOT doing it

  int i = 0;
  while (i < len - 1) {
    char c = getc();
    if (c == '\n') {
      if (do_echo)
        putchar(c);
      break;
    }
    if (c == 8) {
      if (i > 0) {
        i--;
        if (do_echo) {
          putchar(c);
          putchar(' ');
          putchar(c);
        }
      }
      continue;
    }
    buf[i++] = c;
    if (do_echo)
      putchar(c);
  }
  buf[i] = '\0';
  return i;
}

// ===================== Helpers =====================

static void build_abs_path(const char *rel, char *abs) {
  int i;
  if (rel[0] == '/') {
    for (i = 0; rel[i] && i < 255; i++)
      abs[i] = rel[i];
    abs[i] = '\0';
  } else {
    for (i = 0; cwd[i] && i < 255; i++)
      abs[i] = cwd[i];
    if (i > 1 && cwd[i - 1] != '/' && i < 255)
      abs[i++] = '/';
    for (int j = 0; rel[j] && i < 255; j++, i++)
      abs[i] = rel[j];
    abs[i] = '\0';
  }
}

// ===================== Command handlers =====================

static void cmd_ls(const char *rel_path, int long_format) {
  char abs_path[256];
  if (rel_path[0] == '\0') {
    int i;
    for (i = 0; cwd[i] && i < 255; i++)
      abs_path[i] = cwd[i];
    abs_path[i] = '\0';
  } else {
    build_abs_path(rel_path, abs_path);
  }
  DIR *dir = opendir(abs_path);
  if (!dir) {
    if (errno == ENOTDIR)
      printf("ls: not a directory\n");
    else
      printf("ls: cannot access\n");
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    /* Skip dot-files by default (like Ubuntu ls) */
    if (entry->d_name[0] == '.')
      continue;

    if (long_format) {
      // Get file info via stat
      char entry_path[512];
      int ei = 0;
      for (int j = 0; abs_path[j] && ei < 255; j++)
        entry_path[ei++] = abs_path[j];
      if (ei > 0 && entry_path[ei - 1] != '/')
        entry_path[ei++] = '/';
      for (int j = 0; entry->d_name[j] && ei < 511; j++)
        entry_path[ei++] = entry->d_name[j];
      entry_path[ei] = '\0';

      struct stat st;
      if (stat(entry_path, &st) == 0) {
        if (S_ISDIR(st.st_mode))
          printf("drwxr-xr-x");
        else
          printf("-rw-r--r--");
        printf(" %u", S_ISDIR(st.st_mode) ? 2 : 1);
        printf(" root root");
        printf(" %u", (unsigned)st.st_size);
        printf(" %s", entry->d_name);
      } else {
        printf("?????????? 1 root root 0 %s", entry->d_name);
      }
      putchar('\n');
    } else {
      printf("%s\n", entry->d_name);
    }
  }

  closedir(dir);
}

static void cmd_cat(const char *rel_path) {
  char abs_path[256];
  build_abs_path(rel_path, abs_path);

  int fd = open(abs_path, O_RDONLY);
  if (fd < 0) {
    printf("cat: cannot open\n");
    return;
  }

  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < n; i++)
      putchar(buf[i]);
  }

  close(fd);
}

static void cmd_cd(const char *rel_path) {
  char abs_path[256];
  build_abs_path(rel_path, abs_path);

  // Verify it's a directory via stat
  struct stat st;
  if (stat(abs_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    printf("cd: not a directory\n");
    return;
  }

  int i;
  for (i = 0; i < 255 && abs_path[i]; i++)
    cwd[i] = abs_path[i];
  cwd[i] = '\0';
  int len = i;
  if (len > 1 && cwd[len - 1] == '/') {
    cwd[len - 1] = '\0';
    len--;
  }
}

static void cmd_pwd() { printf("%s\n", cwd); }

static void cmd_touch(const char *rel_path) {
  char abs_path[256];
  build_abs_path(rel_path, abs_path);

  int fd = open(abs_path, O_WRONLY | O_CREAT);
  if (fd < 0) {
    if (errno == ENOENT)
      printf("touch: parent directory not found\n");
    else if (errno == ENOTDIR)
      printf("touch: parent is not a directory\n");
    else
      printf("touch: error\n");
    return;
  }
  close(fd);
}

static void cmd_echo(const char *args) {
  // Parse: echo TEXT > FILE  or  echo TEXT >> FILE
  const char *text = args;
  const char *redirect = NULL;
  int append = 0;
  // Find > or >>
  for (const char *p = args; *p; p++) {
    if (*p == '>') {
      if (*(p + 1) == '>') {
        append = 1;
        redirect = p + 2;
      } else {
        append = 0;
        redirect = p + 1;
      }
      break;
    }
  }
  if (!redirect) {
    printf("Usage: echo TEXT > FILE  or  echo TEXT >> FILE\n");
    return;
  }

  // Extract text (everything before >)
  int text_len = (int)(redirect - args - 1 - append);
  if (text_len < 0)
    text_len = 0;
  while (text_len > 0 && text[text_len - 1] == ' ')
    text_len--;

  // Skip spaces after >>
  while (*redirect == ' ')
    redirect++;

  char abs_path[256];
  build_abs_path(redirect, abs_path);

  int flags = O_WRONLY | O_CREAT;
  if (append)
    flags |= O_APPEND;
  int fd = open(abs_path, flags);
  if (fd < 0) {
    printf("echo: cannot open %s\n", abs_path);
    return;
  }

  ssize_t written = write(fd, text, (size_t)text_len);
  close(fd);

  if (written < 0)
    printf("echo: write error\n");
}

static void cmd_mkdir(const char *rel_path) {
  char abs_path[256];
  build_abs_path(rel_path, abs_path);

  if (mkdir(abs_path, 0755) != 0) {
    if (errno == ENOENT)
      printf("mkdir: parent directory not found\n");
    else if (errno == EEXIST)
      printf("mkdir: already exists\n");
    else if (errno == ENOMEM)
      printf("mkdir: no free cluster\n");
    else
      printf("mkdir: error\n");
  }
}

// Execute a file: spawn(path), wait
__attribute__((used)) static void exec_path(const char *rel_path) {
  char abs_path[256];
  build_abs_path(rel_path, abs_path);

  pid_t child_pid = spawn(abs_path);
  if (child_pid < 0) {
    printf("%s: spawn failed\n", rel_path);
    return;
  }

  int32_t exit_code = 0;
  pid_t result = waitpid(child_pid, &exit_code, 0);
  if (result < 0) {
    printf("%s: waitpid failed\n", rel_path);
    return;
  }
}

// ===================== Command parsing =====================

typedef void (*cmd_fn)(const char *args);

struct cmd_entry {
  const char *name;
  cmd_fn handler;
  int min_args;
};

static const cmd_entry cmds[] = {
    {"cat", cmd_cat, 1},
    {"touch", cmd_touch, 1},
    {"mkdir", cmd_mkdir, 1},
    {"echo", cmd_echo, 1},
};

// ===================== Main =====================

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  // VFS is in-kernel, no need to wait for fs_driver
  printf("shell: ready\n");

  // Become session leader and set controlling terminal
  setsid();
  ioctl(0, TIOCSCTTY, 0);

#ifdef TEST
  pid_t test_pid = fork();
  if (test_pid == 0) {
    execve("/test/test_runner.elf", NULL, NULL);
    _exit(127);
  }
  int test_status = 0;
  waitpid(test_pid, &test_status, 0);
#endif

  char line[256];

  while (1) {
    printf("> ");
    int len = readline(line, sizeof(line));
    if (len == 0)
      continue;

    const char *p = line;
    while (*p == ' ')
      p++;

    char cmd_name[256];
    int ci = 0;
    while (*p && *p != ' ' && ci < 255)
      cmd_name[ci++] = *p++;
    cmd_name[ci] = '\0';

    while (*p == ' ')
      p++;

    // Built-in commands
    if (strcmp(cmd_name, "ls") == 0) {
      int long_fmt = 0;
      const char *arg = p;
      if (*p == '-' && *(p + 1) == 'l') {
        long_fmt = 1;
        p += 2;
        while (*p == ' ')
          p++;
        arg = p;
      }
      cmd_ls(arg, long_fmt);
      continue;
    }

    if (strcmp(cmd_name, "cd") == 0) {
      if (*p == '\0')
        cmd_cd("/");
      else
        cmd_cd(p);
      continue;
    }

    if (strcmp(cmd_name, "pwd") == 0) {
      cmd_pwd();
      continue;
    }

    if (strcmp(cmd_name, "clear") == 0) {
      printf("\033[2J\033[H");
      continue;
    }

    if (strcmp(cmd_name, "h") == 0) {
      printf("ls [-l] [path]  - list directory\n");
      printf("cat <path>      - read file\n");
      printf("cd <path>       - change directory\n");
      printf("pwd             - print working directory\n");
      printf("touch <path>    - create empty file\n");
      printf("echo TEXT > FILE  - write text to file\n");
      printf("mkdir <path>    - create directory\n");
      printf("clear           - clear screen\n");
      printf("<path>          - execute ELF file\n");
      printf("h               - show help\n");
      continue;
    }

    // Check built-in command table
    bool found_builtin = false;
    for (int i = 0; i < (int)(sizeof(cmds) / sizeof(cmds[0])); i++) {
      if (strcmp(cmd_name, cmds[i].name) == 0) {
        if (cmds[i].handler) {
          if (cmds[i].min_args > 0 && *p == '\0') {
            printf("%s: missing argument\n", cmds[i].name);
          } else {
            cmds[i].handler(p);
          }
        }
        found_builtin = true;
        break;
      }
    }
    if (found_builtin)
      continue;

    // Not a built-in command — try to execute as ELF file
    char abs_path[256];
    build_abs_path(cmd_name, abs_path);

    struct stat st;
    if (stat(abs_path, &st) != 0) {
      printf("%s: not found\n", cmd_name);
      continue;
    }

    int fd = open(abs_path, O_RDONLY);
    if (fd < 0) {
      printf("%s: cannot open\n", cmd_name);
      continue;
    }
    char magic[4];
    if (read(fd, magic, 4) != 4 || magic[0] != 0x7f || magic[1] != 'E' ||
        magic[2] != 'L' || magic[3] != 'F') {
      close(fd);
      printf("%s: not an executable\n", cmd_name);
      continue;
    }
    close(fd);

    exec_path(cmd_name);
  }
}
