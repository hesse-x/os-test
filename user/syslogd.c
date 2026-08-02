/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* Minimal local syslog daemon. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Strip the syslog <PRI> prefix (e.g. "<13>") so /var/log/messages holds
 * standard "Mon DD HH:MM:SS ident[pid]: msg" lines, not raw "<pri>...".
 * musl's _vsyslog emits "<%d>%s ..." (third_party/musl src/misc/syslog.c),
 * so skip a leading '<', digits, then the closing '>'. Unknown framing is
 * left untouched. Returns the offset into buf where the message proper
 * begins. */
static size_t strip_pri(const char *buf, size_t n) {
  if (n < 3 || buf[0] != '<')
    return 0;
  size_t i = 1;
  while (i < n && buf[i] >= '0' && buf[i] <= '9')
    i++;
  if (i < n && buf[i] == '>' && i + 1 <= n)
    return i + 1;
  return 0;
}

static int bind_log_socket(void) {
  struct sockaddr_un addr;
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, "/dev/log");
  unlink(addr.sun_path);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int main(void) {
  char buf[1024];
  mkdir("/var", 0755);
  mkdir("/var/log", 0755);
  int sock = bind_log_socket();
  if (sock < 0)
    return 1;
  int out = open("/var/log/messages", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (out < 0)
    return 1;
  int ready = open("/run/syslogd.ready", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (ready >= 0)
    close(ready);
  for (;;) {
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
      size_t off = strip_pri(buf, (size_t)n);
      write(out, buf + off, (size_t)n - off);
      continue;
    }
    /* recv <= 0: EINTR retries immediately; transient errors back off with a
     * short sleep to avoid a busy-spin burning CPU if the socket errors. A
     * persistent failure (e.g. socket closed) keeps looping with backoff
     * rather than spinning — logging is best-effort, we never exit. */
    if (errno == EINTR)
      continue;
    struct timespec ts = {0, 10 * 1000 * 1000}; /* 10ms */
    nanosleep(&ts, NULL);
  }
}
