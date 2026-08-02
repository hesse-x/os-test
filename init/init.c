/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// init process — PID 2 (VFS in-kernel)
// Spawns kbd_driver, evdev, terminal, and optionally test_runner
// Adopts orphan children and reaps them via waitpid(-1)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/device.h>
#include <sys/process.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xos/ipc.h>
#include <xos/syscall_ext.h>
#include <xos/unistd_ext.h>

static int spawn_service(const char *path) {
  printf("spawn: %s\n", path);
  pid_t pid = spawn(path);
  return (pid > 0) ? (int)pid : -1;
}

// spawn_with_fd: fork+exec passes fd to the child as fd 3 (socket activation).
// This OS's FD_CLOEXEC is per-struct-file (not per-fd); after fork parent and
// child share the same struct file, so the Linux per-fd trick ("parent holds
// CLOEXEC to prevent leakage, child clears CLOEXEC to keep fd") is not
// possible. So CLOEXEC is not set: the listen fd leaks to sibling processes
// (evdev/terminal) but is harmless (one extra unused fd). The child only does
// dup2 to land fd 3 + close(4..31) to clear fd-table garbage + execve.
// Returns -1 on failure.
static int spawn_with_fd(const char *path, int listen_fd) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    // Child: normalize to fd 3 + close leaked fds + execve
    if (dup2(listen_fd, 3) < 0)
      _exit(127);
    for (int fd = 4; fd < 32; fd++)
      close(fd);
    execve(path, NULL, NULL);
    _exit(127);
  }
  return (int)pid;
}

// create_udev_socket: create an AF_UNIX listen socket bound to
// /run/udev/socket. Returns the listen fd (expected fd 3) or -1 (on failure
// udevd falls back to self-bind). getsockname does not exist in this OS; we
// rely on socket() returning the lowest free fd (stdio holds 0/1/2 -> fd 3),
// and dup2 forces it back to fd 3 if occupied.
static int create_udev_socket(void) {
  // Create /run/udev first. Without mkdir, vfs_mknod_socket path_walk_parent
  // fails -> bind falls back to the hash table. mkdir is idempotent (EEXIST
  // ignored).
  mkdir("/run/udev", 0755); // /run is already a tmpfs mount, mkdir works

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, "/run/udev/socket", sizeof(addr.sun_path) - 1);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 8) < 0) {
    close(fd);
    return -1;
  }
  // Normalize to fd 3: if socket() returned something other than 3 (some
  // other fd occupied it), dup2 forces it back to fd 3
  if (fd != 3) {
    if (dup2(fd, 3) < 0) {
      close(fd);
      return -1;
    }
    close(fd);
    fd = 3;
  }
  return fd;
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
  mkdir("/var", 0755);
  mkdir("/var/log", 0755);
  unlink("/run/syslogd.ready");
  int syslogd_pid = spawn_service("/usr/bin/syslogd");
  for (int i = 0; i < 200 && access("/run/syslogd.ready", F_OK) < 0; i++)
    usleep(10 * 1000);

  // 2. Spawn evdev (keyboard event source + EVIOCG* ioctl query), wait for
  //    /dev/input/event0. Replaces the old kbd driver.
  printf("init: spawning evdev\n");
  int evdev_pid = spawn_service("/driver/evdev.dev");
  wait_dev_ready("/dev/input/event0");
  printf("init: evdev ready\n");

  // 3. Spawn udevd (socket activation: init creates the listen socket and
  //    passes it to udevd)
  printf("init: spawning udevd\n");
  int listen_fd = create_udev_socket(); // <0: udevd falls back to self-bind
  int udevd_pid;
  if (listen_fd >= 0) {
    udevd_pid = spawn_with_fd("/usr/bin/udevd", listen_fd);
  } else {
    udevd_pid = spawn_service("/usr/bin/udevd");
  }

  // 4. Spawn terminal (which spawns shell internally)
  // Settled gate: poll /run/udev/settled (created by udevd after coldplug
  // drain) so the db is ready before spawning terminal, otherwise libinput
  // reads empty ID_INPUT_* and judges unsupported -> terminal block is a
  // black screen (root cause in fix.md). Wait at most ~2s; on timeout spawn
  // anyway (degrades to original behavior, does not block boot; aligned with
  // systemd udev settle, diverging via a file marker + init polling rather
  // than an IPC command channel).
  for (int i = 0; i < 200; i++) {
    if (access("/run/udev/settled", F_OK) == 0)
      break;
    usleep(10 * 1000);
  }
  printf("init: spawning terminal\n");
  spawn_service("/usr/bin/terminal");
  printf("init: terminal spawned\n");

// 5. Adopt orphans + reap children + udevd/evdev crash monitoring (R1)
#define RESTART_SEC 1
#define START_LIMIT_BURST 5
  int udevd_crashes = 0;
  int evdev_crashes = 0;
  int syslogd_crashes = 0;
  while (1) {
    int status;
    pid_t ret = waitpid(-1, &status, 0);
    if (ret < 0)
      continue;
    int crashed =
        WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);

    if (ret == syslogd_pid) {
      if (!crashed)
        continue;
      if (++syslogd_crashes > START_LIMIT_BURST)
        continue;
      sleep(RESTART_SEC);
      unlink("/run/syslogd.ready");
      syslogd_pid = spawn_service("/usr/bin/syslogd");
      continue;
    }

    if (ret == udevd_pid) {
      if (!crashed) {
        udevd_crashes = 0;
        continue;
      }
      udevd_crashes++;
      if (udevd_crashes > START_LIMIT_BURST) {
        printf("init: udevd crashed %d times, giving up respawn\n",
               udevd_crashes);
        continue;
      }
      printf("init: udevd crashed (count %d), respawn in %ds\n", udevd_crashes,
             RESTART_SEC);
      sleep(RESTART_SEC);
      udevd_pid = (listen_fd >= 0) ? spawn_with_fd("/usr/bin/udevd", listen_fd)
                                   : spawn_service("/usr/bin/udevd");
      continue;
    }

    if (ret == evdev_pid) {
      if (!crashed) {
        evdev_crashes = 0;
        continue;
      }
      evdev_crashes++;
      if (evdev_crashes > START_LIMIT_BURST) {
        printf("init: evdev crashed %d times, giving up respawn\n",
               evdev_crashes);
        continue;
      }
      printf("init: evdev crashed (count %d), respawn in %ds\n", evdev_crashes,
             RESTART_SEC);
      sleep(RESTART_SEC);
      evdev_pid = spawn_service("/driver/evdev.dev");
      if (evdev_pid > 0)
        wait_dev_ready("/dev/input/event0");
      continue;
    }
    // Reap other children, ignored
  }

  return 0;
}
