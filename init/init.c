/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// init process — PID 2 (VFS in-kernel)
// Spawns kbd_driver, evdev, terminal, and optionally test_runner
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

  // 2. Spawn evdev (keyboard event source + EVIOCG* ioctl query), wait for
  //    /dev/input/event0. Replaces the old kbd driver.
  printf("init: spawning evdev\n");
  spawn_service("/driver/evdev.dev");
  wait_dev_ready("/dev/input/event0");
  printf("init: evdev ready\n");

  // 3. Spawn udevd (device event daemon, subscribes to netlink uevent group)
  printf("init: spawning udevd\n");
  spawn_service("/usr/bin/udevd");

  // 4. Spawn terminal (which spawns shell internally)
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
