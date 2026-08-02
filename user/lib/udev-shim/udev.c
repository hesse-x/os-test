/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "libudev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h> // open (musl/repo fcntl.h; old unistd.h no longer declares it)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xos/errno.h"

#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xos/ioctl.h>
#include <xos/socket.h>

#define EVDEV_BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x) - 1) / EVDEV_BITS_PER_LONG) + 1)
#define LONG(x) ((x) / EVDEV_BITS_PER_LONG)
#define OFF(x) ((x) % EVDEV_BITS_PER_LONG)

// Path-seat mode backend: minimal udev shim that only supports the calls
// libinput's path-seat mode uses.

static int scan_devices(void);
static int device_is_keyboard(const char *devnode);
static struct udev_device *find_device_by_devnum(dev_t devnum);

// Simple device table
#define MAX_UDEV_DEVICES 16
static struct udev_device *udev_device_table[MAX_UDEV_DEVICES];
static int udev_device_count;
static int udev_table_scanned;

// ======================== udev ========================

struct udev *udev_new(void) {
  struct udev *u = calloc(1, sizeof(struct udev));
  if (u)
    u->refcount = 1;
  return u;
}

struct udev *udev_ref(struct udev *udev) {
  if (udev)
    udev->refcount++;
  return udev;
}

void udev_unref(struct udev *udev) {
  if (udev && --udev->refcount == 0)
    free(udev);
}

// ======================== internal helpers ========================

static void scan_devices_if_needed(void) {
  if (udev_table_scanned)
    return;
  udev_table_scanned = 1;
  udev_device_count = 0;
  memset(udev_device_table, 0, sizeof(udev_device_table));
  scan_devices();
}

// Trailing '*' wildcard match (libinput commonly calls
// udev_enumerate_add_match_sysname("event*")). Only supports trailing
// wildcards of the form "event*"; without '*', degrades to exact strcmp.
// Returns 1 on match / 0 on no match (userspace has no stdbool.h, use int).
static int match_pattern(const char *pattern, const char *name) {
  if (!pattern || !name)
    return 0;
  const char *star = strchr(pattern, '*');
  if (!star)
    return strcmp(pattern, name) == 0;
  size_t prefix_len = star - pattern;
  return strncmp(name, pattern, prefix_len) == 0;
}

static int device_is_keyboard(const char *devnode) {
  // Attempt to detect keyboard via EVIOCGBIT
  int fd = open(devnode, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "udev-shim: open(%s) failed errno=%d\n", devnode, errno);
    return 0;
  }
  unsigned long bits[NBITS(EV_MAX + 1)];
  memset(bits, 0, sizeof(bits));
  int rc = ioctl(fd, EVIOCGBIT(0, sizeof(bits)), bits);
  if (rc < 0)
    fprintf(stderr, "udev-shim: EVIOCGBIT(%s) rc=%d errno=%d\n", devnode, rc,
            errno);
  close(fd);
  if (rc < 0)
    return 0;
  return !!(bits[LONG(EV_KEY)] & (1UL << OFF(EV_KEY)));
}

static struct udev_device *find_device_by_devnum(dev_t devnum) {
  scan_devices_if_needed();
  for (int i = 0; i < udev_device_count; i++) {
    if (udev_device_table[i] && udev_device_table[i]->devnum == devnum)
      return udev_device_table[i];
  }
  return NULL;
}

static struct udev_device *create_udev_device(struct udev *udev,
                                              const char *devnode) {
  (void)udev;
  struct stat st;
  if (stat(devnode, &st) < 0)
    return NULL;

  struct udev_device *d = calloc(1, sizeof(struct udev_device));
  if (!d)
    return NULL;
  d->refcount = 1;
  d->initialized = 1;
  d->devnum = st.st_rdev;

  strncpy(d->devnode, devnode, sizeof(d->devnode) - 1);
  d->devnode[sizeof(d->devnode) - 1] = '\0';

  // subsystem is fixed to "input" (scan_devices only scans /dev/input);
  // fill it first so syspath construction can reference it.
  strncpy(d->subsystem, "input", sizeof(d->subsystem) - 1);
  d->subsystem[sizeof(d->subsystem) - 1] = '\0';

  // Build syspath: /sys/class/<subsystem>/<sysname> (matches the kernel
  // sysfs mount path).
  const char *basename = strrchr(devnode, '/');
  if (!basename)
    basename = devnode;
  else
    basename++;

  // subsystem is currently fixed to "input"; keep the branch structure so
  // extending to drm and other subsystems later is straightforward.
  if (strcmp(d->subsystem, "drm") == 0)
    snprintf(d->syspath, sizeof(d->syspath), "/sys/class/drm/%s", basename);
  else
    snprintf(d->syspath, sizeof(d->syspath), "/sys/class/%s/%s", d->subsystem,
             basename);

  strncpy(d->sysname, basename, sizeof(d->sysname) - 1);
  d->sysname[sizeof(d->sysname) - 1] = '\0';

  // Detect if input or evdev type
  const char *s = strrchr(devnode, '/');
  if (s && strstr(s, "event") != NULL) {
    strncpy(d->devtype, "evdev", sizeof(d->devtype) - 1);
    d->devtype[sizeof(d->devtype) - 1] = '\0';
  }

  return d;
}

static int scan_devices(void) {
  DIR *dir = opendir("/dev/input");
  if (!dir)
    return 0;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL &&
         udev_device_count < MAX_UDEV_DEVICES) {
    if (strncmp(entry->d_name, "event", 5) != 0)
      continue;
    char devnode[64];
    snprintf(devnode, sizeof(devnode), "/dev/input/%s", entry->d_name);
    struct stat st;
    if (stat(devnode, &st) < 0)
      continue;
    // Check if keyboard
    if (!device_is_keyboard(devnode))
      continue;
    struct udev_device *d = create_udev_device(NULL, devnode);
    if (d)
      udev_device_table[udev_device_count++] = d;
  }
  closedir(dir);
  return udev_device_count;
}

// ======================== udev_device ========================

struct udev_device *udev_device_new_from_syspath(struct udev *udev,
                                                 const char *syspath) {
  (void)udev;
  scan_devices_if_needed();
  for (int i = 0; i < udev_device_count; i++) {
    if (udev_device_table[i] &&
        strcmp(udev_device_table[i]->syspath, syspath) == 0)
      return udev_device_ref(udev_device_table[i]);
  }
  return NULL;
}

struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type,
                                                dev_t devnum) {
  (void)udev;
  (void)type;
  // Shortcut: try to stat /dev/input/event* to find the device
  struct udev_device *d = find_device_by_devnum(devnum);
  if (d) {
    return udev_device_ref(d);
  }

  // Fallback: scan /dev/input/ for matching devnum
  DIR *dir = opendir("/dev/input");
  if (!dir)
    return NULL;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0)
      continue;
    char devnode[64];
    snprintf(devnode, sizeof(devnode), "/dev/input/%s", entry->d_name);
    struct stat st;
    if (stat(devnode, &st) < 0)
      continue;
    if (st.st_rdev != devnum)
      continue;
    closedir(dir);
    d = create_udev_device(udev, devnode);
    if (d && udev_device_count < MAX_UDEV_DEVICES)
      udev_device_table[udev_device_count++] = udev_device_ref(d);
    return d;
  }
  closedir(dir);
  return NULL;
}

struct udev_device *
udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem,
                                       const char *sysname) {
  (void)udev;
  scan_devices_if_needed();
  for (int i = 0; i < udev_device_count; i++) {
    if (udev_device_table[i] &&
        strcmp(udev_device_table[i]->subsystem, subsystem) == 0 &&
        strcmp(udev_device_table[i]->sysname, sysname) == 0)
      return udev_device_ref(udev_device_table[i]);
  }
  return NULL;
}

struct udev_device *udev_device_ref(struct udev_device *udev_device) {
  if (udev_device)
    udev_device->refcount++;
  return udev_device;
}

void udev_device_unref(struct udev_device *udev_device) {
  if (!udev_device)
    return;
  if (--udev_device->refcount == 0) {
    // If this is in the static table, leave it there (don't free)
    // Only free if not in the table
    int in_table = 0;
    for (int i = 0; i < udev_device_count; i++) {
      if (udev_device_table[i] == udev_device) {
        in_table = 1;
        break;
      }
    }
    if (!in_table)
      free(udev_device);
  }
}

// udev.c — udev_device_get_property_value landed here (userspace C, int as
// bool)
const char *udev_device_get_property_value(struct udev_device *udev_device,
                                           const char *key) {
  if (!udev_device || !key)
    return NULL;

  // monitor device: properties arrive as pipe KV and are stored in
  // device->props (mirrors Linux libudev's properties hashmap — monitor-path
  // properties arrive with the uevent, are stored in memory and read from
  // memory, not the db; §5.3/§6 grill decision went with the pipe KV path, so
  // remove events don't depend on the db still being present). nprops>0 means
  // monitor source: look up the table and return on hit.
  if (udev_device->nprops > 0) {
    for (int i = 0; i < udev_device->nprops; i++) {
      if (strcmp(udev_device->props[i].key, key) == 0)
        return udev_device->props[i].value;
    }
    return NULL; // not in the monitor table → NULL (no db fallback, mirrors
                 // Linux: a monitor device's properties come only from uevent
                 // KV)
  }

  // Direct-read device (nprops==0): go through the db (mirrors Linux libudev
  // reading /run/udev/data/<key>).
  char key_str[32], path[80];
  snprintf(key_str, sizeof(key_str), "%u", (unsigned)udev_device->devnum);
  snprintf(path, sizeof(path), "/run/udev/data/%s", key_str);

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL; // db file missing (udevd not started / hasn't written) → NULL
                 // (degraded, §5.2)

  static char db_buf[2048]; // valid for a single call; caller must copy
                            // immediately (mirrors Linux libudev)
  ssize_t n = read(fd, db_buf, sizeof(db_buf) - 1);
  close(fd);
  if (n <= 0)
    return NULL;
  db_buf[n] = '\0';

  // Parse KEY=VALUE\n lines looking for the requested key.
  char *line = db_buf;
  while (line && *line) {
    char *eol = strchr(line, '\n');
    if (eol)
      *eol = '\0';
    char *eq = strchr(line, '=');
    if (eq) {
      *eq = '\0';
      if (strcmp(line, key) == 0) {
        return eq + 1; // points into db_buf; caller must copy immediately
      }
      *eq = '=';
    }
    line = eol ? eol + 1 : NULL;
  }
  return NULL;
}

const char *udev_device_get_devnode(struct udev_device *udev_device) {
  return udev_device ? udev_device->devnode : NULL;
}

const char *udev_device_get_syspath(struct udev_device *udev_device) {
  return udev_device ? udev_device->syspath : NULL;
}

const char *udev_device_get_sysname(struct udev_device *udev_device) {
  return udev_device ? udev_device->sysname : NULL;
}

int udev_device_get_is_initialized(struct udev_device *udev_device) {
  return udev_device ? udev_device->initialized : 0;
}

dev_t udev_device_get_devnum(struct udev_device *udev_device) {
  return udev_device ? udev_device->devnum : 0;
}

const char *udev_device_get_action(struct udev_device *udev_device) {
  // monitor device returns "add"/"remove"/"change"; a direct-read device has
  // action[0]=='\0' and returns NULL (Q3).
  if (!udev_device || udev_device->action[0] == '\0')
    return NULL;
  return udev_device->action;
}

const char *udev_device_get_subsystem(struct udev_device *udev_device) {
  return udev_device ? udev_device->subsystem : NULL;
}

const char *udev_device_get_devtype(struct udev_device *udev_device) {
  return udev_device ? udev_device->devtype : NULL;
}

const char *udev_device_get_sysattr_value(struct udev_device *udev_device,
                                          const char *sysattr) {
  if (!udev_device || !sysattr)
    return NULL;

  // syspath is already "/sys/class/input/event0" (after P1 fix).
  // evdev layout: name lives at the device dir root, bustype/vendor/product/
  // version live under the id/ subdir (see devtmpfs.c:661
  // target = (i==0) ? devdir : iddir).
  // drm and other subsystems keep attributes at the class-dir root.
  char path[128];
  if (strcmp(udev_device->subsystem, "input") == 0) {
    if (strcmp(sysattr, "name") == 0)
      snprintf(path, sizeof(path), "%s/%s", udev_device->syspath, sysattr);
    else
      snprintf(path, sizeof(path), "%s/id/%s", udev_device->syspath, sysattr);
  } else {
    snprintf(path, sizeof(path), "%s/%s", udev_device->syspath, sysattr);
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL;

  // Single-call-valid static buffer; the next call overwrites it (matches
  // Linux libudev behavior — caller must copy immediately).
  static char attr_buf[256];
  ssize_t n = read(fd, attr_buf, sizeof(attr_buf) - 1);
  close(fd);
  if (n <= 0)
    return NULL;

  attr_buf[n] = '\0';
  if (n > 0 && attr_buf[n - 1] == '\n')
    attr_buf[n - 1] = '\0';
  return attr_buf;
}

struct udev_list_entry *
udev_device_get_properties_list_entry(struct udev_device *udev_device) {
  (void)udev_device;
  return NULL;
}

struct udev_list_entry *
udev_device_get_sysattr_list_entry(struct udev_device *udev_device) {
  (void)udev_device;
  return NULL;
}

const char *udev_device_get_driver(struct udev_device *udev_device) {
  (void)udev_device;
  return NULL;
}

struct udev_device *udev_device_get_parent(struct udev_device *udev_device) {
  (void)udev_device;
  return NULL;
}

struct udev_device *
udev_device_get_parent_with_subsystem_devtype(struct udev_device *udev_device,
                                              const char *subsystem,
                                              const char *devtype) {
  struct udev_device *parent = udev_device_get_parent(udev_device);
  while (parent) {
    const char *parent_subsystem = udev_device_get_subsystem(parent);
    const char *parent_devtype = udev_device_get_devtype(parent);
    if ((!subsystem ||
         (parent_subsystem && strcmp(parent_subsystem, subsystem) == 0)) &&
        (!devtype || (parent_devtype && strcmp(parent_devtype, devtype) == 0)))
      return parent;
    parent = udev_device_get_parent(parent);
  }
  return NULL;
}

struct udev *udev_device_get_udev(struct udev_device *udev_device) {
  (void)udev_device;
  return NULL;
}

// ======================== list_entry ========================

struct udev_list_entry *
udev_list_entry_get_next(struct udev_list_entry *list_entry) {
  return list_entry ? list_entry->next : NULL;
}

const char *udev_list_entry_get_name(struct udev_list_entry *list_entry) {
  return list_entry ? list_entry->name : NULL;
}

const char *udev_list_entry_get_value(struct udev_list_entry *list_entry) {
  return list_entry ? list_entry->value : NULL;
}

// ======================== monitor (real impl: AF_UNIX + SCM_RIGHTS + pipe)
// ========================

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev,
                                                   const char *name) {
  (void)name; // mirrors Linux taking "udev"; this OS has no "kernel" direct
              // option
  struct udev_monitor *m = calloc(1, sizeof(struct udev_monitor));
  if (m) {
    m->udev = udev_ref(udev);
    m->sock_fd = -1;
    m->pipe_fd = -1;
    m->subscribed = 0;
  }
  return m;
}

int udev_monitor_filter_add_match_subsystem_devtype(
    struct udev_monitor *udev_monitor, const char *subsystem,
    const char *devtype) {
  (void)udev_monitor;
  (void)subsystem;
  (void)devtype;
  return 0; // no-op stub this round (Q5), TODO left in place
}

int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor) {
  if (!udev_monitor)
    return -EINVAL;
  if (udev_monitor->subscribed)
    return 0; // idempotent

  int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sfd < 0)
    return -errno;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, "/run/udev/socket", sizeof(addr.sun_path) - 1);
  if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    // udevd not started / socket missing → -ENOENT; path-seat still works
    // (§5.4)
    close(sfd);
    return -ENOENT;
  }

  // Receive SCM_RIGHTS: udevd sends back the pipe rd fd (§4.4 step 5-6).
  // Carries a 1-byte dummy iov.
  char dummy;
  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  struct iovec iov;
  iov.iov_base = &dummy;
  iov.iov_len = 1;
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);

  if (recvmsg(sfd, &msg, 0) < 0) {
    close(sfd);
    return -errno;
  }

  int got_fd = -1;
  struct cmsghdr *cmsg;
  // musl's CMSG_NXTHDR compares size_t against a signed pointer difference;
  // silence the inherent -Wsign-compare under our -Werror gate (musl's own
  // sources build with -Wno-all for the same reason).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
  for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      memcpy(&got_fd, CMSG_DATA(cmsg), sizeof(int));
      break;
    }
  }
#pragma clang diagnostic pop
  if (got_fd < 0) {
    close(sfd);
    return -EPROTO;
  }

  // Close the conn fd (connect == subscribe, Q5; after taking the pipe fd we
  // no longer communicate over it).
  close(sfd);
  udev_monitor->pipe_fd = got_fd;
  udev_monitor->subscribed = 1;
  return 0;
}

int udev_monitor_get_fd(struct udev_monitor *udev_monitor) {
  // Returns the pipe rd fd (epoll-able), mirroring Linux monitor fd semantics.
  return udev_monitor ? udev_monitor->pipe_fd : -1;
}

struct udev_device *
udev_monitor_receive_device(struct udev_monitor *udev_monitor) {
  if (!udev_monitor || udev_monitor->pipe_fd < 0)
    return NULL;

  char buf[4096];
  ssize_t len = read(udev_monitor->pipe_fd, buf, sizeof(buf) - 1);
  if (len <= 0)
    return NULL; // 0=EOF (udevd crash / closed pipe), <0=EAGAIN/error
  buf[len] = '\0';

  struct udev_device *d = calloc(1, sizeof(struct udev_device));
  if (!d)
    return NULL;
  d->refcount = 1;
  d->initialized = 1;

  // Parse \0-separated key=value (same parser origin as netlink uevent, Q4).
  char *p = buf, *end = buf + len;
  while (p < end) {
    char *eq = strchr(p, '=');
    if (!eq) {
      int sl = (int)strlen(p);
      p += sl + 1;
      continue;
    }
    *eq = '\0';
    char *key = p, *val = eq + 1;
    if (strcmp(key, "ACTION") == 0)
      strncpy(d->action, val, sizeof(d->action) - 1);
    else if (strcmp(key, "DEVNAME") == 0)
      strncpy(d->devnode, val, sizeof(d->devnode) - 1);
    else if (strcmp(key, "DEVPATH") == 0)
      strncpy(d->syspath, val, sizeof(d->syspath) - 1);
    else if (strcmp(key, "SUBSYSTEM") == 0)
      strncpy(d->subsystem, val, sizeof(d->subsystem) - 1);
    else if (strcmp(key, "DEVTYPE") == 0)
      strncpy(d->devtype, val, sizeof(d->devtype) - 1);
    else if (strcmp(key, "DEVNUM") == 0)
      d->devnum = (dev_t)strtoul(val, NULL, 10);
    else {
      // Non-identifier key → property (ID_INPUT_*, ID_SEAT, WL_*, MOUSE_DPI,
      // etc.). Store into the device's property table (mirrors Linux libudev:
      // a monitor device's properties arrive with the uevent KV, are held in
      // memory, and get_property_value reads this table, not the db — §5.3/§6
      // grill decision: take the pipe KV path; remove events don't depend on
      // the db still being present). Drop the property if the table is full
      // (32 slots is enough).
      if (d->nprops < UDEV_DEV_PROPS_MAX) {
        strncpy(d->props[d->nprops].key, key, UDEV_PROP_KEYLEN - 1);
        d->props[d->nprops].key[UDEV_PROP_KEYLEN - 1] = '\0';
        strncpy(d->props[d->nprops].value, val, UDEV_PROP_VALLEN - 1);
        d->props[d->nprops].value[UDEV_PROP_VALLEN - 1] = '\0';
        d->nprops++;
      }
    }
    p = val + strlen(val) + 1;
  }

  // sysname = last segment of syspath (last DEVPATH segment, e.g. "event0").
  // Take it from the raw syspath first; normalize syspath itself afterwards.
  const char *slash = strrchr(d->syspath, '/');
  strncpy(d->sysname, slash ? slash + 1 : d->syspath, sizeof(d->sysname) - 1);
  d->sysname[sizeof(d->sysname) - 1] = '\0';

  // Normalize syspath to "/sys/class/<subsystem>/<sysname>", matching the
  // scan path in create_udev_device (:153). The kernel netlink sends DEVPATH
  // as a bare relative path ("input/event0", no /sys/ prefix); leaving it as
  // is would make it unequal to the scan table's "/sys/class/input/event0",
  // causing libinput to:
  //   - fail dedup in filter_duplicates (udev-seat.c:62), replaying add events;
  //   - fail the evdev_device_have_same_syspath check (evdev.c:2274), silently
  //     goto err and log "failed to create input device".
  if (d->subsystem[0] && d->sysname[0]) {
    char norm[256];
    snprintf(norm, sizeof(norm), "/sys/class/%s/%s", d->subsystem, d->sysname);
    strncpy(d->syspath, norm, sizeof(d->syspath) - 1);
    d->syspath[sizeof(d->syspath) - 1] = '\0';
  }
  return d;
}

void udev_monitor_unref(struct udev_monitor *udev_monitor) {
  if (!udev_monitor)
    return;
  if (udev_monitor->sock_fd >= 0)
    close(udev_monitor->sock_fd);
  if (udev_monitor->pipe_fd >= 0)
    close(udev_monitor->pipe_fd);
  udev_unref(udev_monitor->udev);
  free(udev_monitor);
}

// ======================== enumerate (no-op stubs) ========================

struct udev_enumerate *udev_enumerate_new(struct udev *udev) {
  (void)udev;
  struct udev_enumerate *e = calloc(1, sizeof(struct udev_enumerate));
  return e;
}

void udev_enumerate_unref(struct udev_enumerate *udev_enumerate) {
  if (!udev_enumerate)
    return;
  struct udev_list_entry *cur = udev_enumerate->devices;
  while (cur) {
    struct udev_list_entry *next = cur->next;
    free(cur);
    cur = next;
  }
  free(udev_enumerate);
}

int udev_enumerate_add_match_subsystem(struct udev_enumerate *udev_enumerate,
                                       const char *subsystem) {
  if (!udev_enumerate || !subsystem)
    return -EINVAL;
  strncpy(udev_enumerate->subsystem_filter, subsystem,
          sizeof(udev_enumerate->subsystem_filter) - 1);
  udev_enumerate
      ->subsystem_filter[sizeof(udev_enumerate->subsystem_filter) - 1] = '\0';
  return 0;
}

int udev_enumerate_add_match_sysname(struct udev_enumerate *udev_enumerate,
                                     const char *sysname) {
  if (!udev_enumerate || !sysname)
    return -EINVAL;
  strncpy(udev_enumerate->sysname_filter, sysname,
          sizeof(udev_enumerate->sysname_filter) - 1);
  udev_enumerate->sysname_filter[sizeof(udev_enumerate->sysname_filter) - 1] =
      '\0';
  return 0;
}

int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate) {
  if (!udev_enumerate)
    return -EINVAL;

  // Known-subsystem whitelist. Without a subsystem_filter, scan all of them.
  const char *subsystems[] = {"input", "drm", NULL};
  const char *only_subsys = udev_enumerate->subsystem_filter[0]
                                ? udev_enumerate->subsystem_filter
                                : NULL;

  // Clear stale results (supports repeated scan).
  struct udev_list_entry *cur = udev_enumerate->devices;
  while (cur) {
    struct udev_list_entry *next = cur->next;
    free(cur);
    cur = next;
  }
  udev_enumerate->devices = NULL;

  for (int si = 0; subsystems[si]; si++) {
    if (only_subsys && strcmp(subsystems[si], only_subsys) != 0)
      continue;

    char class_path[64];
    snprintf(class_path, sizeof(class_path), "/sys/class/%s", subsystems[si]);
    DIR *dir = opendir(class_path);
    if (!dir)
      continue;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (entry->d_name[0] == '.')
        continue;
      if (udev_enumerate->sysname_filter[0] &&
          !match_pattern(udev_enumerate->sysname_filter, entry->d_name))
        continue;

      char syspath[128];
      snprintf(syspath, sizeof(syspath), "/sys/class/%s/%s", subsystems[si],
               entry->d_name);

      struct udev_list_entry *le = calloc(1, sizeof(struct udev_list_entry));
      if (!le)
        continue;
      strncpy(le->name, syspath, sizeof(le->name) - 1);
      le->name[sizeof(le->name) - 1] = '\0';
      le->next = udev_enumerate->devices;
      udev_enumerate->devices = le;
    }
    closedir(dir);
  }

  return 0;
}

struct udev_list_entry *
udev_enumerate_get_list_entry(struct udev_enumerate *udev_enumerate) {
  return udev_enumerate ? udev_enumerate->devices : NULL;
}

char *udev_device_get_property_value_w(char *property, size_t property_size,
                                       struct udev_device *udev_device,
                                       const char *key) {
  const char *val = udev_device_get_property_value(udev_device, key);
  if (!val)
    return NULL;
  size_t len = strlen(val) + 1;
  if (len > property_size)
    return NULL;
  memcpy(property, val, len);
  return property;
}
