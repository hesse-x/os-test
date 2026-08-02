/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct proto_header {
  uint16_t opcode;
  uint16_t size;
};

static int connect_seatd(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, "/run/seatd.sock");
  if (connect(fd, (struct sockaddr *)&addr,
              sizeof(addr.sun_family) + strlen(addr.sun_path) + 1) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int rejected(struct proto_header header) {
  int fd = connect_seatd();
  if (fd < 0 || write(fd, &header, sizeof(header)) != sizeof(header)) {
    if (fd >= 0)
      close(fd);
    return -1;
  }
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLHUP};
  int ret = poll(&pfd, 1, 2000);
  if (ret <= 0) {
    close(fd);
    return -1;
  }
  char response[32];
  ssize_t n = read(fd, response, sizeof(response));
  close(fd);
  return n <= 0 || (n >= (ssize_t)sizeof(struct proto_header) &&
                    ((struct proto_header *)response)->opcode == 0xffff)
             ? 0
             : -1;
}

int main(void) {
  if (rejected((struct proto_header){.opcode = 0x7ffe, .size = 0}) < 0 ||
      rejected((struct proto_header){.opcode = 3, .size = 0}) < 0) {
    fprintf(stderr, "seat-protocol-negative: malformed message accepted\n");
    return 1;
  }
  puts("WF6_PROTOCOL_NEGATIVE_PASS");
  return 0;
}
