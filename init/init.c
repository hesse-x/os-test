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
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int spawn_service(const char *path) {
  printf("spawn: %s\n", path);
  pid_t pid = spawn(path);
  return (pid > 0) ? (int)pid : -1;
}

/* spawn_with_fd: fork+exec 传 fd 到子进程 fd 3（socket activation）。
 * 本 OS FD_CLOEXEC 是 per-struct-file（非 per-fd），fork 后父子共享同一 struct
 * file， 无法做到"父持 CLOEXEC 阻泄漏、子清 CLOEXEC 保留 fd"的 Linux per-fd
 * 语义。 故不设 CLOEXEC：listen fd
 * 会泄漏给兄弟进程（evdev/terminal），但无害（多一个不用 的 fd）。子进程只做
 * dup2 归位 fd 3 + close(4..31) 防 fd 表垃圾 + execve。 失败返 -1。 */
static int spawn_with_fd(const char *path, int listen_fd) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    /* 子进程：归一到 fd 3 + 关泄漏 fd + execve */
    if (dup2(listen_fd, 3) < 0)
      _exit(127);
    for (int fd = 4; fd < 32; fd++)
      close(fd);
    execve(path, NULL, NULL);
    _exit(127);
  }
  return (int)pid;
}

/* create_udev_socket：建 AF_UNIX listen socket 绑 /run/udev/socket。
 * 返 listen fd（期望 fd 3）或 -1（失败时 udevd 走自 bind 降级）。
 * getsockname 在本 OS 不存在，靠 socket() 返回最小空闲 fd 约定
 * （stdio 占 0/1/2 → fd 3）；若被占则 dup2 强制归位 fd 3。 */
static int create_udev_socket(void) {
  /* ↓↓↓ 本方案新增:先建 /run/udev 目录 ↓↓↓
   * 现状无 mkdir,vfs_mknod_socket path_walk_parent 失败 → bind 降级 hash 表。
   * mkdir 幂等(EEXIST 忽略)。 */
  mkdir("/run/udev", 0755); /* /run 已是 tmpfs mount,可 mkdir */

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
  /* fd 3 归位：若 socket() 返回非 3（被其它 fd 占），dup2 强制归位 */
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

  // 2. Spawn evdev (keyboard event source + EVIOCG* ioctl query), wait for
  //    /dev/input/event0. Replaces the old kbd driver.
  printf("init: spawning evdev\n");
  int evdev_pid = spawn_service("/driver/evdev.dev");
  wait_dev_ready("/dev/input/event0");
  printf("init: evdev ready\n");

  // 3. Spawn udevd (socket activation: init 建 listen socket 传 udevd)
  printf("init: spawning udevd\n");
  int listen_fd = create_udev_socket(); /* <0 则 udevd 走自 bind 降级 */
  int udevd_pid;
  if (listen_fd >= 0) {
    udevd_pid = spawn_with_fd("/usr/bin/udevd", listen_fd);
  } else {
    udevd_pid = spawn_service("/usr/bin/udevd");
  }

  // 4. Spawn terminal (which spawns shell internally)
  /* settled gate:轮询 /run/udev/settled(udevd coldplug drain 后建),保证 db
   * 就绪再 spawn terminal,否则 libinput 读 ID_INPUT_* 为空判 unsupported →
   * terminal block 黑屏(根因见 fix.md)。最多等 ~2s,超时仍 spawn(退化为原
   * 行为,不阻塞启动;对齐 systemd udev settle,偏离:文件标志 + init 轮询,
   * 无 IPC 命令通道)。 */
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
  while (1) {
    int status;
    pid_t ret = waitpid(-1, &status, 0);
    if (ret < 0)
      continue;
    int crashed =
        WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);

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
    /* 其它子进程收尸，忽略 */
  }

  return 0;
}
