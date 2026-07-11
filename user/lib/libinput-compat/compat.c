/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Stub libevdev functions needed by compiled plugin files
#include <libevdev/libevdev.h>
#include <strings.h>

int libevdev_get_event_value(struct libevdev *evdev, unsigned int type,
                             unsigned int code) {
  (void)evdev;
  (void)type;
  (void)code;
  return 0;
}
int libevdev_enable_event_code(struct libevdev *evdev, unsigned int type,
                               unsigned int code, const void *data) {
  (void)evdev;
  (void)type;
  (void)code;
  (void)data;
  return 0;
}
int libevdev_disable_event_code(struct libevdev *evdev, unsigned int type,
                                unsigned int code) {
  (void)evdev;
  (void)type;
  (void)code;
  return 0;
}
int libevdev_get_num_slots(struct libevdev *evdev) {
  (void)evdev;
  return 0;
}
int libevdev_get_current_slot(struct libevdev *evdev) {
  (void)evdev;
  return 0;
}
int libevdev_get_slot_value(struct libevdev *evdev, unsigned int slot,
                            unsigned int type, unsigned int code) {
  (void)evdev;
  (void)slot;
  (void)type;
  (void)code;
  return 0;
}
int libevdev_change_fd(struct libevdev *evdev, int fd) {
  (void)evdev;
  (void)fd;
  return 0;
}

int ffs(int i) {
  (void)i;
  return 0;
}

// Plugin stubs (never called for keyboard devices)
struct libinput_device *
libinput_mouse_plugin_wheel_lowres(struct libinput_device *device);
struct libinput_device *
libinput_mouse_plugin_wheel_lowres(struct libinput_device *device) {
  (void)device;
  return NULL;
}

// Tablet plugin stubs (never called for keyboard devices)
struct libinput_device *
libinput_tablet_plugin_forced_tool(struct libinput_device *device) {
  (void)device;
  return NULL;
}
struct libinput_device *
libinput_tablet_plugin_double_tool(struct libinput_device *device) {
  (void)device;
  return NULL;
}
struct libinput_device *
libinput_tablet_plugin_proximity_timer(struct libinput_device *device) {
  (void)device;
  return NULL;
}
struct libinput_device *
libinput_tablet_plugin_eraser_button(struct libinput_device *device) {
  (void)device;
  return NULL;
}

char *fgets(char *s, int size, FILE *stream) {
  if (size <= 0 || !s || !stream)
    return NULL;
  int i = 0;
  while (i < size - 1) {
    int c = fgetc(stream);
    if (c == EOF) {
      if (i == 0)
        return NULL;
      break;
    }
    s[i++] = (char)c;
    if (c == '\n')
      break;
  }
  s[i] = '\0';
  return s;
}

int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
  DIR *dir = opendir(dirp);
  if (!dir)
    return -1;

  size_t capacity = 32;
  size_t count = 0;
  struct dirent **list = malloc(capacity * sizeof(struct dirent *));
  if (!list) {
    closedir(dir);
    errno = ENOMEM;
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (filter && !filter(entry))
      continue;

    struct dirent *copy = malloc(sizeof(struct dirent));
    if (!copy) {
      for (size_t i = 0; i < count; i++)
        free(list[i]);
      free(list);
      closedir(dir);
      errno = ENOMEM;
      return -1;
    }
    *copy = *entry;

    if (count >= capacity) {
      capacity *= 2;
      struct dirent **newlist =
          realloc(list, capacity * sizeof(struct dirent *));
      if (!newlist) {
        free(copy);
        for (size_t i = 0; i < count; i++)
          free(list[i]);
        free(list);
        closedir(dir);
        errno = ENOMEM;
        return -1;
      }
      list = newlist;
    }
    list[count++] = copy;
  }
  closedir(dir);

  if (compar)
    qsort(list, count, sizeof(struct dirent *),
          (int (*)(const void *, const void *))compar);

  *namelist = list;
  return (int)count;
}
