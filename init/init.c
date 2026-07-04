/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// init process — PID 2 (VFS in-kernel)
// Spawns kbd_driver, terminal, and optionally test_runner
// Adopts orphan children and reaps them via waitpid(-1)
#include "syscall.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/device.h>
#include <sys/ipc.h>
#include <sys/process.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int spawn_service(const char *path) {
  printf("spawn: %s\n", path);
  pid_t pid = spawn(path);
  return (pid > 0) ? (int)pid : -1;
}

static void wait_dev_ready(const char *dev_path) {
  int fd;
  for (int tries = 0;; tries++) {
    printf("wait_dev_ready: calling open %s try=%d\n", dev_path, tries);
    fd = open(dev_path, O_RDWR);
    printf("wait_dev_ready: open %s try=%d ret=%d\n", dev_path, tries, fd);
    if (fd >= 0)
      break;
    printf("wait_dev_ready: calling recv try=%d\n", tries);
    struct recv_msg m;
    recv(&m, NULL, 0, 10);
    printf("wait_dev_ready: recv returned try=%d\n", tries);
  }
  printf("wait_dev_ready: %s opened fd=%d\n", dev_path, fd);
  close(fd);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  // Set up serial as stdin/stdout/stderr first so printf works
  {
    int sfd = open("/dev/serial", O_RDWR);
    if (sfd >= 0) {
      dup2(sfd, 0);
      dup2(sfd, 1);
      dup2(sfd, 2);
      if (sfd > 2)
        close(sfd);
    }
  }

  printf("init: started\n");

  // 2. Spawn kbd_driver, wait for /dev/kbd
  printf("init: spawning kbd_driver\n");
  {
    pid_t p = spawn("/driver/kbd.dev");
    printf("init: spawn kbd returned pid=%d\n", (int)p);
  }
  wait_dev_ready("/dev/kbd");
  printf("init: kbd_driver ready\n");

  // 3. Spawn terminal (which spawns shell internally)
  // /dev/dri/card0 is registered by the kernel (virtio-gpu DRM) — no need
  // to spawn a separate display driver.
  printf("init: spawning terminal\n");
  spawn_service("/usr/bin/terminal");
  printf("init: terminal spawned\n");

  // 5. Adopt orphans + reap children
  while (1) {
    int status;
    waitpid(-1, &status, 0);
  }

  return 0;
}
