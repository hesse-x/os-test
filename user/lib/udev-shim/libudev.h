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

struct udev_list_entry;

#define UDEV_DEV_PROPS_MAX 32
#define UDEV_PROP_KEYLEN 64
#define UDEV_PROP_VALLEN 256
#define UDEV_FRAME_HEADER_SIZE 12
#define UDEV_FRAME_PAYLOAD_MAX 4096
typedef char
    udev_frame_header_must_be_12_bytes[UDEV_FRAME_HEADER_SIZE == 12 ? 1 : -1];

struct udev_device {
  int refcount;
  struct udev *udev;
  char devnode[256];
  char syspath[256];
  char sysname[64];
  char subsystem[32];
  char devtype[32];
  char action[16]; // monitor device's ACTION (add/remove/change); empty for
                   // direct-read device
  dev_t devnum;
  int initialized;
  // Property table (KV received over the pipe for monitor devices:
  // ID_INPUT_*, ID_SEAT, etc.). Mirrors Linux libudev's properties hashmap —
  // monitor-path properties arrive with the uevent KV, are stored in device
  // memory, and get_property_value reads this table (not the db). Array
  // stands in for a hashmap: freestanding userspace has no ready hashmap.
  // 32 slots cover ID_INPUT* (12) + ID_SEAT + a few WL_*, MOUSE_DPI, etc.
  // nprops=0 means no properties (direct-read device, db fallback).
  struct {
    char key[UDEV_PROP_KEYLEN];
    char value[UDEV_PROP_VALLEN];
  } props[UDEV_DEV_PROPS_MAX];
  int nprops;
  int properties_loaded;
  struct udev_list_entry *properties_list;
};

struct udev_list_entry {
  char name[128];
  char value[256];
  struct udev_list_entry *next;
};

struct udev_monitor {
  struct udev *udev; // held via udev_ref, released on unref
  int sock_fd; // AF_UNIX conn fd (held briefly after connect, closed once pipe
               // fd is taken)
  int pipe_fd; // pipe rd fd received via SCM_RIGHTS (get_fd returns this;
               // epoll-able)
  int subscribed; // set to 1 after enable_receiving (idempotent)
  char subsystem_filter[32];
  char devtype_filter[32];
  unsigned char frame_header[UDEV_FRAME_HEADER_SIZE];
  size_t frame_header_used;
  unsigned char frame_payload[UDEV_FRAME_PAYLOAD_MAX];
  size_t frame_payload_used;
  size_t frame_payload_len;
  int protocol_errors;
};

struct udev_enumerate {
  int refcount;
  struct udev *udev;
  char subsystem_filter[32];       // e.g. "input"; empty string = no filter
  char sysname_filter[64];         // e.g. "event*"; empty string = no filter
  struct udev_list_entry *devices; // scan result list head
};

// Forward declarations needed by cleanup functions
void udev_unref(struct udev *udev);
void udev_device_unref(struct udev_device *udev_device);
void udev_monitor_unref(struct udev_monitor *udev_monitor);
void udev_enumerate_unref(struct udev_enumerate *udev_enumerate);

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

// Mirrors Linux libudev: udev-seat.c:172 uses this macro to iterate
// enumerate results. get_next is already provided by the shim (declared
// above / implemented at udev.c:437); this only adds the foreach macro.
#define udev_list_entry_foreach(list_entry, first_entry)                       \
  for (list_entry = (first_entry); list_entry != NULL;                         \
       list_entry = udev_list_entry_get_next(list_entry))

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
struct udev_device *
udev_device_get_parent_with_subsystem_devtype(struct udev_device *udev_device,
                                              const char *subsystem,
                                              const char *devtype);
struct udev *udev_device_get_udev(struct udev_device *udev_device);

char *udev_device_get_property_value_w(char *property, size_t property_size,
                                       struct udev_device *udev_device,
                                       const char *key);

#ifdef __cplusplus
}
#endif

#endif
