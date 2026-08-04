/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// init process — PID 2 (VFS in-kernel)
// Spawns the core services and terminal, then monitors their fault domains.
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

static int spawn_process(const char *path, char *const argv[],
                         char *const envp[], int pass_fd, int child_fd,
                         mode_t child_umask) {
  printf("spawn: %s\n", path);
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    if (child_umask != (mode_t)-1)
      umask(child_umask);
    if (pass_fd >= 0 && pass_fd != child_fd && dup2(pass_fd, child_fd) < 0)
      _exit(127);
    if (pass_fd >= 0 && fcntl(child_fd, F_SETFD, 0) < 0)
      _exit(127);
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 4)
      max_fd = 1024;
    for (int fd = 3; fd < max_fd; fd++) {
      if (pass_fd >= 0 && fd == child_fd)
        continue;
      close(fd);
    }
    execve(path, argv, envp);
    _exit(127);
  }
  return (int)pid;
}

static int spawn_service(const char *path) {
  char *const argv[] = {(char *)path, NULL};
  return spawn_process(path, argv, NULL, -1, -1, (mode_t)-1);
}

// spawn_with_fd: fork+exec passes fd to the child as fd 3 (socket activation).
// The child normalizes the activation fd to 3, clears its per-fd CLOEXEC bit,
// closes every other non-stdio descriptor, and then execs with explicit argv.
// Returns -1 on failure.
static int spawn_with_fd(const char *path, int listen_fd) {
  char *const argv[] = {(char *)path, NULL};
  return spawn_process(path, argv, NULL, listen_fd, 3, (mode_t)-1);
}

static int start_seatd(int *read_fd) {
  int ready[2];
  if (pipe2(ready, O_CLOEXEC) < 0)
    return -1;
  char *const argv[] = {"/usr/bin/seatd", "-n", "3", "-l", "debug", NULL};
  char *const envp[] = {"SEATD_VTBOUND=0", NULL};
  int pid = spawn_process(argv[0], argv, envp, ready[1], 3, 0077);
  close(ready[1]);
  if (pid < 0) {
    close(ready[0]);
    return -1;
  }
  *read_fd = ready[0];
  return pid;
}

static int wait_seatd_ready(int fd) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  char byte;
  int ok = poll(&pfd, 1, 2000) == 1 && (pfd.revents & POLLIN) &&
           read(fd, &byte, 1) == 1;
  close(fd);
  struct stat st;
  return ok && stat("/run/seatd.sock", &st) == 0 && S_ISSOCK(st.st_mode) ? 0
                                                                         : -1;
}

#ifndef TINYWL
static int start_terminal(void) {
  char *const argv[] = {"/usr/bin/terminal", NULL};
  // Minimal stable session environment (terminal/step1.md §3.2): terminal and
  // its shell descendants must not run with an empty environ. Terminal-specific
  // entries (TERM/COLORTERM) are set by the terminal itself.
  char *const envp[] = {"LIBSEAT_BACKEND=seatd",
                        "SEATD_SOCK=/run/seatd.sock",
                        "PATH=/usr/local/bin:/usr/bin:/bin",
                        "HOME=/root",
                        "USER=root",
                        "SHELL=/bin/sh",
                        "LANG=C.UTF-8",
                        "XDG_RUNTIME_DIR=/run",
                        NULL};
  return spawn_process(argv[0], argv, envp, -1, -1, (mode_t)-1);
}
#endif

#if defined(TEST) || defined(TINYWL)
static int start_tinywl(void) {
  /* -s: tinywl spawns the wallpaper client once the Wayland socket is up. */
  char *const argv[] = {"/usr/bin/tinywl", "-s", "/usr/bin/wallpaper", NULL};
  char *const envp[] = {"LIBSEAT_BACKEND=seatd",
                        "SEATD_SOCK=/run/seatd.sock",
                        "XDG_RUNTIME_DIR=/run",
                        "WLR_BACKENDS=drm,libinput",
                        "WLR_RENDERER=gles2",
                        "GBM_BACKENDS_PATH=/lib",
                        "XKB_CONFIG_ROOT=/usr/share/X11/xkb",
                        "PATH=/usr/local/bin:/usr/bin:/bin",
                        NULL};
  return spawn_process(argv[0], argv, envp, -1, -1, (mode_t)-1);
}
#endif

static void stop_process(int pid) {
  if (pid <= 0)
    return;
  kill(pid, SIGTERM);
  for (int i = 0; i < 200; i++) {
    if (waitpid(pid, NULL, WNOHANG) == pid)
      return;
    usleep(10 * 1000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
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

  // 4. Wait for the udev coldplug database before starting a display owner.
  // Settled gate: poll /run/udev/settled (created by udevd after coldplug
  // drain) so the db is ready before spawning terminal, otherwise libinput
  // reads empty ID_INPUT_* and judges unsupported -> terminal block is a
  // black screen (root cause in fix.md). Wait at most ~2s; on timeout spawn
  // anyway (degrades to original behavior, does not block boot; aligned with
  // systemd udev settle, diverging via a file marker + init polling rather
  // than an IPC command channel).
  int udev_settled = 0;
  for (int i = 0; i < 200; i++) {
    if (access("/run/udev/settled", F_OK) == 0) {
      udev_settled = 1;
      break;
    }
    usleep(10 * 1000);
  }
  (void)udev_settled;

  int seatd_ready_fd = -1;
  int seatd_pid = start_seatd(&seatd_ready_fd);
  if (seatd_pid < 0 || wait_seatd_ready(seatd_ready_fd) < 0) {
    printf("init: fatal: seatd readiness timeout\n");
    stop_process(seatd_pid);
    for (;;)
      pause();
  }
  printf("init: seatd ready pid=%d\n", seatd_pid);
#ifdef TINYWL
  printf("init: spawning tinywl\n");
  int direct_tinywl_pid = start_tinywl();
  if (direct_tinywl_pid < 0) {
    printf("init: fatal: tinywl spawn failed\n");
    stop_process(seatd_pid);
    for (;;)
      pause();
  }
  printf("init: tinywl spawned pid=%d\n", direct_tinywl_pid);
#else
  printf("init: spawning terminal\n");
  int terminal_pid = start_terminal();
  if (terminal_pid < 0) {
    printf("init: fatal: terminal spawn failed\n");
    stop_process(seatd_pid);
    for (;;)
      pause();
  }
  printf("init: terminal spawned pid=%d\n", terminal_pid);
#endif

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

#ifdef TINYWL
    if (ret == direct_tinywl_pid || ret == seatd_pid) {
      printf("init: tinywl fault pid=%d status=%d; stopping seat domain\n",
             (int)ret, status);
      if (ret == direct_tinywl_pid)
        stop_process(seatd_pid);
      else
        stop_process(direct_tinywl_pid);
      for (;;)
        pause();
    }
#else
    if (ret == terminal_pid || ret == seatd_pid) {
      if (ret == seatd_pid) {
        printf("init: seatd fault status=%d; stopping terminal\n", status);
        stop_process(terminal_pid);
        for (;;)
          pause();
      }
#ifdef TEST
      printf("init: tests done, launching tinywl for TW-3 E2E\n");
      int tinywl_pid = start_tinywl();
      if (tinywl_pid < 0) {
        perror("init: spawn tinywl failed");
        for (;;)
          pause();
      }
      int tinywl_status;
      if (waitpid(tinywl_pid, &tinywl_status, 0) < 0)
        perror("init: wait for tinywl failed");
      else
        printf("init: tinywl exited status=%d\n", tinywl_status);
      for (;;)
        pause();
#else
      printf("init: terminal fault pid=%d status=%d; stopping seat domain\n",
             (int)ret, status);
      stop_process(seatd_pid);
      for (;;)
        pause();
#endif
    }
#endif

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
