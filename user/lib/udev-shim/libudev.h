/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef LIBUDEV_H
#define LIBUDEV_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct udev {
  int refcount;
};

struct udev_device {
  int refcount;
  char devnode[256];
  char syspath[256];
  char sysname[64];
  char subsystem[32];
  char devtype[32];
  dev_t devnum;
  int initialized;
};

struct udev_list_entry {
  char name[128];
  char value[256];
  struct udev_list_entry *next;
};

struct udev_monitor {
  int fd;
};

struct udev_enumerate {
  int dummy;
};

// Forward declarations needed by cleanup functions
void udev_unref(struct udev *udev);
void udev_device_unref(struct udev_device *udev_device);
void udev_monitor_unref(struct udev_monitor *udev_monitor);
void udev_enumerate_unref(struct udev_enumerate *udev_enumerate);

// Cleanup helpers for _unref_/_cleanup_ macros from libinput's util-mem.h
static inline void udev_unrefp(struct udev **p) {
  if (p && *p)
    udev_unref(*p);
}

static inline void udev_device_unrefp(struct udev_device **p) {
  if (p && *p)
    udev_device_unref(*p);
}

static inline void udev_enumerate_unrefp(struct udev_enumerate **p) {
  if (p && *p)
    udev_enumerate_unref(*p);
}

static inline void udev_monitor_unrefp(struct udev_monitor **p) {
  if (p && *p)
    udev_monitor_unref(*p);
}

struct udev *udev_new(void);
struct udev *udev_ref(struct udev *udev);
void udev_unref(struct udev *udev);

struct udev_device *udev_device_new_from_syspath(struct udev *udev,
                                                 const char *syspath);
struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type,
                                                dev_t devnum);
struct udev_device *
udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem,
                                       const char *sysname);
struct udev_device *udev_device_ref(struct udev_device *udev_device);
void udev_device_unref(struct udev_device *udev_device);

const char *udev_device_get_property_value(struct udev_device *udev_device,
                                           const char *key);
const char *udev_device_get_devnode(struct udev_device *udev_device);
const char *udev_device_get_syspath(struct udev_device *udev_device);
const char *udev_device_get_sysname(struct udev_device *udev_device);
int udev_device_get_is_initialized(struct udev_device *udev_device);
dev_t udev_device_get_devnum(struct udev_device *udev_device);
const char *udev_device_get_action(struct udev_device *udev_device);
const char *udev_device_get_subsystem(struct udev_device *udev_device);
const char *udev_device_get_devtype(struct udev_device *udev_device);
const char *udev_device_get_sysattr_value(struct udev_device *udev_device,
                                          const char *sysattr);
struct udev_list_entry *
udev_device_get_properties_list_entry(struct udev_device *udev_device);
struct udev_list_entry *
udev_device_get_sysattr_list_entry(struct udev_device *udev_device);

struct udev_list_entry *
udev_list_entry_get_next(struct udev_list_entry *list_entry);
const char *udev_list_entry_get_name(struct udev_list_entry *list_entry);
const char *udev_list_entry_get_value(struct udev_list_entry *list_entry);

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev,
                                                   const char *name);
int udev_monitor_filter_add_match_subsystem_devtype(
    struct udev_monitor *udev_monitor, const char *subsystem,
    const char *devtype);
int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor);
int udev_monitor_get_fd(struct udev_monitor *udev_monitor);
struct udev_device *
udev_monitor_receive_device(struct udev_monitor *udev_monitor);
void udev_monitor_unref(struct udev_monitor *udev_monitor);

struct udev_enumerate *udev_enumerate_new(struct udev *udev);
void udev_enumerate_unref(struct udev_enumerate *udev_enumerate);
int udev_enumerate_add_match_subsystem(struct udev_enumerate *udev_enumerate,
                                       const char *subsystem);
int udev_enumerate_add_match_sysname(struct udev_enumerate *udev_enumerate,
                                     const char *sysname);
int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate);
struct udev_list_entry *
udev_enumerate_get_list_entry(struct udev_enumerate *udev_enumerate);

const char *udev_device_get_driver(struct udev_device *udev_device);
struct udev_device *udev_device_get_parent(struct udev_device *udev_device);
struct udev *udev_device_get_udev(struct udev_device *udev_device);

char *udev_device_get_property_value_w(char *property, size_t property_size,
                                       struct udev_device *udev_device,
                                       const char *key);

#ifdef __cplusplus
}
#endif

#endif
