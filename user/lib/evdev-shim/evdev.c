/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "libevdev/libevdev.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <xos/input.h>

#define EVDEV_BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1) / EVDEV_BITS_PER_LONG) + 1)
#define LONG(x) ((x) / EVDEV_BITS_PER_LONG)
#define OFF(x) ((x) % EVDEV_BITS_PER_LONG)

struct libevdev {
  int fd;
  char name[256];
  struct input_id id;
  unsigned long caps_bits[EV_MAX + 1][NBITS(KEY_MAX + 1)];
  unsigned long prop_bits[NBITS(INPUT_PROP_MAX + 1)];
  int clock_id;
  int has_ev_syn;
};

int libevdev_new_from_fd(int fd, struct libevdev **evdev) {
  struct libevdev *d = (struct libevdev *)calloc(1, sizeof(struct libevdev));
  if (!d)
    return -ENOMEM;

  d->fd = fd;
  d->clock_id = CLOCK_MONOTONIC;

  // Query capabilities via ioctl
  int ver;
  if (ioctl(fd, EVIOCGVERSION, &ver) < 0)
    goto fail;
  if (ioctl(fd, EVIOCGID, &d->id) < 0)
    goto fail;

  char tmp[256];
  memset(tmp, 0, sizeof(tmp));
  if (ioctl(fd, EVIOCGNAME(sizeof(tmp)), tmp) < 0)
    goto fail;
  strncpy(d->name, tmp, sizeof(d->name) - 1);
  d->name[sizeof(d->name) - 1] = '\0';

  // Query property bits
  unsigned long prop[NBITS(INPUT_PROP_MAX + 1)];
  memset(prop, 0, sizeof(prop));
  if (ioctl(fd, EVIOCGPROP(sizeof(prop)), prop) >= 0) {
    for (unsigned int i = 0; i < NBITS(INPUT_PROP_MAX + 1); i++)
      d->prop_bits[i] = prop[i];
  }

  // Query event type bits (EV_SYN = 0, EV_KEY = 1, ...)
  unsigned long evtype[NBITS(EV_MAX + 1)];
  memset(evtype, 0, sizeof(evtype));
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evtype)), evtype) < 0)
    goto fail;

  for (int ev = 0; ev <= EV_MAX; ev++) {
    if (!(evtype[LONG(ev)] & (1UL << OFF(ev))))
      continue;
    if (ev == EV_SYN) {
      d->has_ev_syn = 1;
      continue;
    }
    memset(d->caps_bits[ev], 0, sizeof(d->caps_bits[ev]));
    ioctl(fd, EVIOCGBIT(ev, sizeof(d->caps_bits[ev])), d->caps_bits[ev]);
  }

  *evdev = d;
  return 0;

fail:
  free(d);
  *evdev = NULL;
  return -errno;
}

void libevdev_free(struct libevdev *evdev) {
  if (evdev) {
    if (evdev->fd >= 0)
      close(evdev->fd);
    free(evdev);
  }
}

int libevdev_get_fd(struct libevdev *evdev) { return evdev->fd; }

const char *libevdev_get_name(struct libevdev *evdev) { return evdev->name; }

int libevdev_has_event_code(struct libevdev *evdev, unsigned int type,
                            unsigned int code) {
  if (type > EV_MAX)
    return 0;
  if (type == EV_SYN)
    return evdev->has_ev_syn;
  if (LONG(code) >= NBITS(KEY_MAX + 1))
    return 0;
  return !!(evdev->caps_bits[type][LONG(code)] & (1UL << OFF(code)));
}

int libevdev_next_event(struct libevdev *evdev, unsigned int flags,
                        struct input_event *ev) {
  struct input_event buf;
  ssize_t n;

  if (flags & LIBEVDEV_READ_FLAG_BLOCKING) {
    n = read(evdev->fd, &buf, sizeof(buf));
  } else {
    // Non-blocking: temporarily set O_NONBLOCK
    int fl = fcntl(evdev->fd, F_GETFL, 0);
    if (fl >= 0 && !(fl & O_NONBLOCK))
      fcntl(evdev->fd, F_SETFL, fl | O_NONBLOCK);
    n = read(evdev->fd, &buf, sizeof(buf));
    if (fl >= 0 && !(fl & O_NONBLOCK))
      fcntl(evdev->fd, F_SETFL, fl);
  }

  if (n < 0)
    return -errno;
  if ((size_t)n < sizeof(buf))
    return -EAGAIN;

  *ev = buf;
  return LIBEVDEV_READ_STATUS_SUCCESS;
}

void libevdev_set_clock_id(struct libevdev *evdev, int clock_id) {
  evdev->clock_id = clock_id;
  ioctl(evdev->fd, EVIOCSCLOCKID, &clock_id);
}

const struct input_absinfo *libevdev_get_abs_info(struct libevdev *evdev,
                                                  unsigned int code) {
  (void)evdev;
  (void)code;
  return NULL;
}

int libevdev_grab(struct libevdev *evdev, int grab) {
  return ioctl(evdev->fd, EVIOCGRAB, &grab);
}

void libevdev_set_log_function(
    enum libevdev_log_priority priority,
    void (*log_func)(enum libevdev_log_priority priority, void *data,
                     const char *file, int line, const char *func,
                     const char *format, va_list args),
    void *data) {
  (void)priority;
  (void)log_func;
  (void)data;
}

const char *libevdev_event_type_get_name(unsigned int type) {
  (void)type;
  return "unknown";
}

const char *libevdev_event_code_get_name(unsigned int type, unsigned int code) {
  (void)type;
  (void)code;
  return "unknown";
}

int libevdev_event_type_from_name(const char *name) {
  (void)name;
  return -1;
}

int libevdev_event_code_from_name(unsigned int type, const char *name) {
  (void)type;
  (void)name;
  return -1;
}

int libevdev_event_type_from_code(unsigned int code) {
  (void)code;
  return -1;
}

unsigned int libevdev_event_type_get_max(unsigned int type) {
  (void)type;
  return 0;
}

int libevdev_property_from_name(const char *name) {
  (void)name;
  return -1;
}

const char *libevdev_property_get_name(unsigned int prop) {
  (void)prop;
  return "unknown";
}
