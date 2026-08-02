/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <libseat.h>
#include <linux/input.h>

struct session {
  struct libseat *seat;
  int active;
  int fatal;
  int card_id;
  int card_fd;
  int input_id;
  int input_fd;
};

static void close_device(struct session *s, int *id, int *fd) {
  if (*id > 0 && s->seat)
    (void)libseat_close_device(s->seat, *id);
  if (*fd >= 0)
    close(*fd);
  *id = -1;
  *fd = -1;
}

static void enable_seat(struct libseat *seat, void *data) {
  (void)seat;
  struct session *s = data;
  if (s->active)
    s->fatal = 1;
  s->active = 1;
}

static void disable_seat(struct libseat *seat, void *data) {
  struct session *s = data;
  if (!s->active) {
    s->fatal = 1;
    return;
  }
  close_device(s, &s->input_id, &s->input_fd);
  close_device(s, &s->card_id, &s->card_fd);
  s->active = 0;
  if (libseat_disable_seat(seat) < 0)
    s->fatal = 1;
}

static const struct libseat_seat_listener listener = {
    .enable_seat = enable_seat,
    .disable_seat = disable_seat,
};

static void session_init(struct session *s) {
  memset(s, 0, sizeof(*s));
  s->card_id = s->input_id = -1;
  s->card_fd = s->input_fd = -1;
}

static int dispatch_until(struct session *s, int want_active) {
  for (int elapsed = 0; elapsed < 2000; elapsed += 50) {
    if (s->fatal)
      return -1;
    if (s->active == want_active)
      return 0;
    if (libseat_dispatch(s->seat, 50) < 0)
      return -1;
  }
  errno = ETIMEDOUT;
  return -1;
}

static int open_session(struct session *s, int wait_active) {
  session_init(s);
  s->seat = libseat_open_seat(&listener, s);
  if (!s->seat)
    return -1;
  const char *name = libseat_seat_name(s->seat);
  if (!name || strcmp(name, "seat0") != 0) {
    errno = ENODEV;
    return -1;
  }
  return wait_active ? dispatch_until(s, 1) : 0;
}

static void close_session(struct session *s) {
  close_device(s, &s->input_id, &s->input_fd);
  close_device(s, &s->card_id, &s->card_fd);
  if (s->seat)
    (void)libseat_close_seat(s->seat);
  s->seat = NULL;
}

static int check_environment(void) {
  const char *backend = getenv("LIBSEAT_BACKEND");
  const char *socket = getenv("SEATD_SOCK");
  if (!backend || strcmp(backend, "seatd") != 0 || !socket ||
      strcmp(socket, "/run/seatd.sock") != 0) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int open_and_check_card(struct session *s) {
  s->card_id = libseat_open_device(s->seat, "/dev/dri/card0", &s->card_fd);
  if (s->card_id <= 0 || s->card_fd < 0)
    return -1;

  int probe = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (probe < 0)
    return -1;
  errno = 0;
  int ret = ioctl(probe, DRM_IOCTL_SET_MASTER, 0);
  int saved = errno;
  close(probe);
  if (ret == 0 || saved != EBUSY) {
    errno = saved ? saved : EPROTO;
    return -1;
  }
  if (ioctl(s->card_fd, DRM_IOCTL_SET_MASTER, 0) < 0)
    return -1;
  return 0;
}

static int basic(void) {
  struct session s;
  session_init(&s);
  if (check_environment() < 0)
    goto fail;
  if (open_session(&s, 1) < 0)
    goto fail_session;
  if (open_and_check_card(&s) < 0)
    goto fail_session;

  s.input_id = libseat_open_device(s.seat, "/dev/input/event0", &s.input_fd);
  if (s.input_id <= 0 || s.input_fd < 0)
    goto fail_session;
  int version = 0;
  if (ioctl(s.input_fd, EVIOCGVERSION, &version) < 0 || version == 0)
    goto fail_session;
  struct pollfd pfd = {.fd = s.input_fd, .events = POLLIN};
  if (poll(&pfd, 1, 0) < 0)
    goto fail_session;

  close_session(&s);
  puts("WF6_BASIC_PASS");
  return 0;

fail_session:
  close_session(&s);
fail:
  fprintf(stderr, "seat-session-smoke: basic failed: %s\n", strerror(errno));
  return 1;
}

static int verify_release(void) {
  int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (fd < 0 || ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0 ||
      ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) < 0) {
    if (fd >= 0)
      close(fd);
    fprintf(stderr, "seat-session-smoke: release probe failed: %s\n",
            strerror(errno));
    return 1;
  }
  close(fd);
  puts("WF6_RELEASE_PASS");
  return 0;
}

static int handoff(void) {
  int parent_to_peer[2], peer_to_parent[2];
  if (pipe(parent_to_peer) < 0 || pipe(peer_to_parent) < 0)
    return 1;
  pid_t peer = fork();
  if (peer < 0)
    return 1;
  if (peer == 0) {
    close(parent_to_peer[1]);
    close(peer_to_parent[0]);
    char byte;
    if (read(parent_to_peer[0], &byte, 1) != 1)
      _exit(2);
    struct session secondary;
    if (open_session(&secondary, 0) < 0 ||
        write(peer_to_parent[1], "R", 1) != 1 ||
        dispatch_until(&secondary, 1) < 0 ||
        open_and_check_card(&secondary) < 0) {
      close_session(&secondary);
      _exit(3);
    }
    close_session(&secondary);
    (void)write(peer_to_parent[1], "P", 1);
    _exit(0);
  }

  close(parent_to_peer[0]);
  close(peer_to_parent[1]);
  struct session primary;
  int rc = 1;
  char byte;
  if (open_session(&primary, 1) < 0 || open_and_check_card(&primary) < 0 ||
      write(parent_to_peer[1], "G", 1) != 1 ||
      read(peer_to_parent[0], &byte, 1) != 1 ||
      libseat_switch_session(primary.seat, 2) < 0 ||
      dispatch_until(&primary, 0) < 0)
    goto out;
  struct pollfd pfd = {.fd = peer_to_parent[0], .events = POLLIN};
  if (poll(&pfd, 1, 2000) == 1 && read(peer_to_parent[0], &byte, 1) == 1 &&
      byte == 'P')
    rc = 0;
out:
  close_session(&primary);
  if (rc)
    kill(peer, SIGKILL);
  int status = 0;
  waitpid(peer, &status, 0);
  if (rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    puts("WF6_HANDOFF_PASS");
    return 0;
  }
  fprintf(stderr, "seat-session-smoke: handoff failed\n");
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s basic|handoff|verify-release|verify-recovery\n",
            argv[0]);
    return 2;
  }
  if (strcmp(argv[1], "basic") == 0)
    return basic();
  if (strcmp(argv[1], "handoff") == 0)
    return handoff();
  if (strcmp(argv[1], "verify-release") == 0)
    return verify_release();
  if (strcmp(argv[1], "verify-recovery") == 0)
    return basic();
  return 2;
}
