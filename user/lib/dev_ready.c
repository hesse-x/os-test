/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <unistd.h>

#include <sys/ipc.h>
#include <xos/fcntl.h>
#include <xos/syscall_nums.h>

void wait_dev_ready(const char *dev_path) {
  int fd;
  for (int tries = 0;; tries++) {
    fd = open(dev_path, O_RDWR);
    if (fd >= 0)
      break;
    struct recv_msg m;
    recv(&m, NULL, 0, 10);
  }
  close(fd);
}
