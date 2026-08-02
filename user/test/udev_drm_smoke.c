/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libudev.h"

static int smoke_enumerate(struct udev *udev) {
  struct udev_enumerate *enumerate = udev_enumerate_new(udev);
  if (!enumerate || udev_enumerate_add_match_subsystem(enumerate, "drm") < 0 ||
      udev_enumerate_add_match_sysname(enumerate, "card[0-9]*") < 0 ||
      udev_enumerate_scan_devices(enumerate) < 0) {
    fprintf(stderr, "udev-drm-smoke: enumerate setup failed errno=%d\n", errno);
    udev_enumerate_unref(enumerate);
    return 1;
  }
  int primary_count = 0;
  struct udev_list_entry *entry;
  udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
    const char *syspath = udev_list_entry_get_name(entry);
    struct udev_device *device = udev_device_new_from_syspath(udev, syspath);
    if (!device)
      continue;
    printf("syspath=%s subsystem=%s sysname=%s devnode=%s devnum=%u\n",
           udev_device_get_syspath(device), udev_device_get_subsystem(device),
           udev_device_get_sysname(device), udev_device_get_devnode(device),
           (unsigned)udev_device_get_devnum(device));
    primary_count++;
    udev_device_unref(device);
  }
  udev_enumerate_unref(enumerate);
  if (primary_count != 1) {
    fprintf(stderr, "udev-drm-smoke: expected one primary card, got %d\n",
            primary_count);
    return 1;
  }
  return 0;
}

static int smoke_device(struct udev *udev, const char *syspath) {
  struct udev_device *device = udev_device_new_from_syspath(udev, syspath);
  if (!device) {
    fprintf(stderr, "udev-drm-smoke: device construction failed errno=%d\n",
            errno);
    return 1;
  }
  struct stat st;
  const char *devnode = udev_device_get_devnode(device);
  int failed = stat(devnode, &st) < 0 ||
               st.st_rdev != udev_device_get_devnum(device) ||
               strcmp(udev_device_get_subsystem(device), "drm") != 0;
  const char *seat = udev_device_get_property_value(device, "ID_SEAT");
  const char *boot_display =
      udev_device_get_sysattr_value(device, "boot_display");
  printf("syspath=%s devnode=%s devnum=%u effective-seat=%s boot_display=%s\n",
         udev_device_get_syspath(device), devnode,
         (unsigned)udev_device_get_devnum(device), seat ? seat : "seat0",
         boot_display ? boot_display : "missing");
  udev_device_unref(device);
  return failed;
}

static int smoke_monitor(struct udev *udev) {
  struct udev_monitor *monitor = udev_monitor_new_from_netlink(udev, "udev");
  if (!monitor ||
      udev_monitor_filter_add_match_subsystem_devtype(monitor, "drm", NULL) <
          0 ||
      udev_monitor_enable_receiving(monitor) < 0) {
    fprintf(stderr, "udev-drm-smoke: monitor setup failed errno=%d\n", errno);
    udev_monitor_unref(monitor);
    return 1;
  }
  int fd = udev_monitor_get_fd(monitor);
  int flags = fcntl(fd, F_GETFL);
  int fdflags = fcntl(fd, F_GETFD);
  int failed = fd < 0 || flags < 0 || !(flags & O_NONBLOCK) || fdflags < 0 ||
               !(fdflags & FD_CLOEXEC);
  printf("monitor-fd=%d nonblock=%d cloexec=%d filter=drm framed=v1\n", fd,
         !!(flags & O_NONBLOCK), !!(fdflags & FD_CLOEXEC));
  udev_monitor_unref(monitor);
  return failed;
}

static int smoke_malformed(struct udev *udev) {
  int fds[2];
  if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0)
    return 1;
  struct udev_monitor *monitor = udev_monitor_new_from_netlink(udev, "udev");
  if (!monitor) {
    close(fds[0]);
    close(fds[1]);
    return 1;
  }
  monitor->pipe_fd = fds[0];
  monitor->subscribed = 1;
  unsigned char frame[14] = {'U', 'D', 'E', 'V', 1, 0,   0,
                             0,   2,   0,   0,   0, 'x', 'y'};
  for (int i = 0; i < 3; i++) {
    if (write(fds[1], frame, sizeof(frame)) != (ssize_t)sizeof(frame))
      return 1;
    errno = 0;
    (void)udev_monitor_receive_device(monitor);
  }
  int failed = monitor->pipe_fd >= 0 || errno != EPROTO;
  close(fds[1]);
  udev_monitor_unref(monitor);
  printf("malformed-payload-fatal=%d errno=%d\n", !failed, errno);
  return failed;
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    fprintf(
        stderr,
        "usage: udev-drm-smoke enumerate|device|monitor|malformed [syspath]\n");
    return 2;
  }
  struct udev *udev = udev_new();
  if (!udev)
    return 1;
  int result;
  if (strcmp(argv[1], "enumerate") == 0 && argc == 2)
    result = smoke_enumerate(udev);
  else if (strcmp(argv[1], "device") == 0 && argc == 3)
    result = smoke_device(udev, argv[2]);
  else if (strcmp(argv[1], "monitor") == 0 && argc == 2)
    result = smoke_monitor(udev);
  else if (strcmp(argv[1], "malformed") == 0 && argc == 2)
    result = smoke_malformed(udev);
  else
    result = 2;
  udev_unref(udev);
  return result;
}
