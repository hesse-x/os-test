/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef LIBEVDEV_H
#define LIBEVDEV_H

#include <linux/input.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct libevdev;

#define LIBEVDEV_READ_FLAG_NORMAL 0x00
#define LIBEVDEV_READ_FLAG_SYNC 0x01
#define LIBEVDEV_READ_FLAG_FORCE_SYNC 0x02
#define LIBEVDEV_READ_FLAG_BLOCKING 0x04

enum libevdev_read_status {
  LIBEVDEV_READ_STATUS_SUCCESS = 0,
  LIBEVDEV_READ_STATUS_SYNC = 1,
};

int libevdev_new_from_fd(int fd, struct libevdev **evdev);
void libevdev_free(struct libevdev *evdev);
int libevdev_get_fd(struct libevdev *evdev);
const char *libevdev_get_name(struct libevdev *evdev);
int libevdev_has_event_code(struct libevdev *evdev, unsigned int type,
                            unsigned int code);
int libevdev_next_event(struct libevdev *evdev, unsigned int flags,
                        struct input_event *ev);
void libevdev_set_clock_id(struct libevdev *evdev, int clock_id);
const struct input_absinfo *libevdev_get_abs_info(struct libevdev *evdev,
                                                  unsigned int code);
int libevdev_grab(struct libevdev *evdev, int grab);

enum libevdev_log_priority {
  LIBEVDEV_LOG_ERROR = 0,
  LIBEVDEV_LOG_INFO = 1,
  LIBEVDEV_LOG_DEBUG = 2,
};

const char *libevdev_property_get_name(unsigned int prop);
void libevdev_set_log_function(
    enum libevdev_log_priority priority,
    void (*log_func)(enum libevdev_log_priority priority, void *data,
                     const char *file, int line, const char *func,
                     const char *format, va_list args),
    void *data);

// libinput internal uses
const char *libevdev_event_type_get_name(unsigned int type);
const char *libevdev_event_code_get_name(unsigned int type, unsigned int code);
int libevdev_event_type_from_name(const char *name);
int libevdev_event_code_from_name(unsigned int type, const char *name);
int libevdev_event_type_from_code(unsigned int code);
unsigned int libevdev_event_type_get_max(unsigned int type);
int libevdev_property_from_name(const char *name);
int libevdev_get_event_value(struct libevdev *evdev, unsigned int type,
                             unsigned int code);
int libevdev_enable_event_code(struct libevdev *evdev, unsigned int type,
                               unsigned int code, const void *data);
int libevdev_disable_event_code(struct libevdev *evdev, unsigned int type,
                                unsigned int code);

#ifdef __cplusplus
}
#endif

#endif
