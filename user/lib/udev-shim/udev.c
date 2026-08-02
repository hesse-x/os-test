/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "libudev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h> // open (musl/repo fcntl.h; old unistd.h no longer declares it)
#include <fnmatch.h>
#include <limits.h>
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

#define MAX_UDEV_DEVICES 16

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

static int match_pattern(const char *pattern, const char *name) {
  return pattern && name && fnmatch(pattern, name, 0) == 0;
}

static int all_digits(const char *s) {
  if (!s || !*s)
    return 0;
  for (; *s; s++)
    if (*s < '0' || *s > '9')
      return 0;
  return 1;
}

static int valid_sysname(const char *subsystem, const char *sysname) {
  if (strcmp(subsystem, "input") == 0)
    return strncmp(sysname, "event", 5) == 0 && all_digits(sysname + 5);
  if (strcmp(subsystem, "drm") == 0) {
    if (strncmp(sysname, "card", 4) == 0)
      return all_digits(sysname + 4);
    if (strncmp(sysname, "renderD", 7) == 0)
      return all_digits(sysname + 7);
  }
  return 0;
}

static struct udev_device *create_udev_device(struct udev *udev,
                                              const char *subsystem,
                                              const char *sysname) {
  if (!udev || !subsystem || !sysname || !valid_sysname(subsystem, sysname)) {
    errno = EINVAL;
    return NULL;
  }

  char devnode[256];
  char syspath[256];
  int n;
  if (strcmp(subsystem, "input") == 0)
    n = snprintf(devnode, sizeof(devnode), "/dev/input/%s", sysname);
  else
    n = snprintf(devnode, sizeof(devnode), "/dev/dri/%s", sysname);
  if (n < 0 || (size_t)n >= sizeof(devnode)) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  n = snprintf(syspath, sizeof(syspath), "/sys/class/%s/%s", subsystem,
               sysname);
  if (n < 0 || (size_t)n >= sizeof(syspath)) {
    errno = ENAMETOOLONG;
    return NULL;
  }

  struct stat st;
  if (stat(syspath, &st) < 0 || stat(devnode, &st) < 0)
    return NULL;
  if (!S_ISCHR(st.st_mode)) {
    errno = ENODEV;
    return NULL;
  }

  struct udev_device *d = calloc(1, sizeof(struct udev_device));
  if (!d)
    return NULL;
  d->refcount = 1;
  d->udev = udev_ref(udev);
  d->initialized = 1;
  d->devnum = st.st_rdev;

  snprintf(d->devnode, sizeof(d->devnode), "%s", devnode);
  snprintf(d->syspath, sizeof(d->syspath), "%s", syspath);
  snprintf(d->subsystem, sizeof(d->subsystem), "%s", subsystem);
  snprintf(d->sysname, sizeof(d->sysname), "%s", sysname);
  if (strcmp(subsystem, "input") == 0)
    snprintf(d->devtype, sizeof(d->devtype), "evdev");

  return d;
}

// ======================== udev_device ========================

struct udev_device *udev_device_new_from_syspath(struct udev *udev,
                                                 const char *syspath) {
  static const char prefix[] = "/sys/class/";
  if (!udev || !syspath || strncmp(syspath, prefix, sizeof(prefix) - 1) != 0 ||
      strstr(syspath, "//") || strstr(syspath, "/../") ||
      strlen(syspath) >= 256) {
    errno = EINVAL;
    return NULL;
  }
  const char *subsystem = syspath + sizeof(prefix) - 1;
  const char *slash = strchr(subsystem, '/');
  if (!slash || slash == subsystem || strchr(slash + 1, '/')) {
    errno = EINVAL;
    return NULL;
  }
  char subsystem_buf[32];
  size_t subsystem_len = (size_t)(slash - subsystem);
  if (subsystem_len >= sizeof(subsystem_buf)) {
    errno = EINVAL;
    return NULL;
  }
  memcpy(subsystem_buf, subsystem, subsystem_len);
  subsystem_buf[subsystem_len] = '\0';
  return create_udev_device(udev, subsystem_buf, slash + 1);
}

struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type,
                                                dev_t devnum) {
  if (!udev || type != 'c' || devnum == 0) {
    errno = EINVAL;
    return NULL;
  }
  const char *subsystems[] = {"input", "drm"};
  struct udev_device *found = NULL;
  for (size_t si = 0; si < sizeof(subsystems) / sizeof(subsystems[0]); si++) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/%s", subsystems[si]);
    DIR *dir = opendir(path);
    if (!dir)
      continue;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (!valid_sysname(subsystems[si], entry->d_name))
        continue;
      struct udev_device *candidate =
          create_udev_device(udev, subsystems[si], entry->d_name);
      if (!candidate)
        continue;
      if (candidate->devnum != devnum) {
        udev_device_unref(candidate);
        continue;
      }
      if (found) {
        fprintf(stderr, "udev-shim DEVICE_STAT duplicate devnum=%u\n",
                (unsigned)devnum);
        udev_device_unref(candidate);
        udev_device_unref(found);
        closedir(dir);
        errno = EEXIST;
        return NULL;
      }
      found = candidate;
    }
    closedir(dir);
  }
  if (!found)
    errno = ENOENT;
  return found;
}

struct udev_device *
udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem,
                                       const char *sysname) {
  return create_udev_device(udev, subsystem, sysname);
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
    struct udev_list_entry *entry = udev_device->properties_list;
    while (entry) {
      struct udev_list_entry *next = entry->next;
      free(entry);
      entry = next;
    }
    udev_unref(udev_device->udev);
    free(udev_device);
  }
}

static void load_device_properties(struct udev_device *d) {
  if (d->properties_loaded)
    return;
  d->properties_loaded = 1;
  char path[80];
  snprintf(path, sizeof(path), "/run/udev/data/%u", (unsigned)d->devnum);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return;
  char buf[UDEV_FRAME_PAYLOAD_MAX + 1];
  ssize_t n = read(fd, buf, sizeof(buf));
  close(fd);
  if (n <= 0 || n > UDEV_FRAME_PAYLOAD_MAX)
    return;
  buf[n] = '\0';
  char *line = buf;
  while (*line && d->nprops < UDEV_DEV_PROPS_MAX) {
    char *eol = strchr(line, '\n');
    if (eol)
      *eol = '\0';
    char *eq = strchr(line, '=');
    size_t key_len = eq ? (size_t)(eq - line) : 0;
    size_t value_len = eq ? strlen(eq + 1) : 0;
    if (key_len > 0 && key_len < UDEV_PROP_KEYLEN &&
        value_len < UDEV_PROP_VALLEN) {
      memcpy(d->props[d->nprops].key, line, key_len);
      d->props[d->nprops].key[key_len] = '\0';
      memcpy(d->props[d->nprops].value, eq + 1, value_len + 1);
      d->nprops++;
    }
    if (!eol)
      break;
    line = eol + 1;
  }
}

const char *udev_device_get_property_value(struct udev_device *udev_device,
                                           const char *key) {
  if (!udev_device || !key)
    return NULL;
  load_device_properties(udev_device);
  for (int i = 0; i < udev_device->nprops; i++)
    if (strcmp(udev_device->props[i].key, key) == 0)
      return udev_device->props[i].value;
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
  if (!udev_device)
    return NULL;
  load_device_properties(udev_device);
  if (!udev_device->properties_list) {
    struct udev_list_entry **tail = &udev_device->properties_list;
    for (int i = 0; i < udev_device->nprops; i++) {
      struct udev_list_entry *entry = calloc(1, sizeof(*entry));
      if (!entry)
        break;
      snprintf(entry->name, sizeof(entry->name), "%s",
               udev_device->props[i].key);
      snprintf(entry->value, sizeof(entry->value), "%s",
               udev_device->props[i].value);
      *tail = entry;
      tail = &entry->next;
    }
  }
  return udev_device->properties_list;
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
  return udev_device ? udev_device->udev : NULL;
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
  if (!udev || !name || strcmp(name, "udev") != 0) {
    errno = EINVAL;
    return NULL;
  }
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
  if (!udev_monitor || !subsystem || !*subsystem)
    return -EINVAL;
  if (strlen(subsystem) >= sizeof(udev_monitor->subsystem_filter) ||
      (devtype && strlen(devtype) >= sizeof(udev_monitor->devtype_filter)))
    return -EINVAL;
  if (udev_monitor->subsystem_filter[0]) {
    if (strcmp(udev_monitor->subsystem_filter, subsystem) == 0 &&
        ((!devtype && !udev_monitor->devtype_filter[0]) ||
         (devtype && strcmp(udev_monitor->devtype_filter, devtype) == 0)))
      return 0;
    return -EOPNOTSUPP;
  }
  snprintf(udev_monitor->subsystem_filter,
           sizeof(udev_monitor->subsystem_filter), "%s", subsystem);
  if (devtype)
    snprintf(udev_monitor->devtype_filter, sizeof(udev_monitor->devtype_filter),
             "%s", devtype);
  return 0;
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
    int saved = errno;
    close(sfd);
    fprintf(stderr,
            "udev-shim MON_CONNECT errno=%d path=/run/udev/socket fatal=1\n",
            saved);
    return -saved;
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

  if (recvmsg(sfd, &msg, MSG_CMSG_CLOEXEC) < 0) {
    int saved = errno;
    close(sfd);
    return -saved;
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

  int flags = fcntl(got_fd, F_GETFL);
  int fdflags = fcntl(got_fd, F_GETFD);
  if (flags < 0 || fdflags < 0 ||
      fcntl(got_fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      fcntl(got_fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) {
    int saved = errno;
    close(got_fd);
    close(sfd);
    return -saved;
  }

  // Close the conn fd (connect == subscribe, Q5; after taking the pipe fd we
  // no longer communicate over it).
  close(sfd);
  udev_monitor->pipe_fd = got_fd;
  udev_monitor->subscribed = 1;
  fprintf(stderr, "udev-shim monitor filter=%s fd=%d framed=v1\n",
          udev_monitor->subsystem_filter[0] ? udev_monitor->subsystem_filter
                                            : "*",
          got_fd);
  return 0;
}

int udev_monitor_get_fd(struct udev_monitor *udev_monitor) {
  // Returns the pipe rd fd (epoll-able), mirroring Linux monitor fd semantics.
  return udev_monitor ? udev_monitor->pipe_fd : -1;
}

struct udev_device *
udev_monitor_receive_device(struct udev_monitor *udev_monitor) {
  if (!udev_monitor || udev_monitor->pipe_fd < 0) {
    errno = EINVAL;
    return NULL;
  }

  for (int budget = 0; budget < 32; budget++) {
    while (udev_monitor->frame_header_used < UDEV_FRAME_HEADER_SIZE) {
      ssize_t n =
          read(udev_monitor->pipe_fd,
               udev_monitor->frame_header + udev_monitor->frame_header_used,
               UDEV_FRAME_HEADER_SIZE - udev_monitor->frame_header_used);
      if (n > 0) {
        udev_monitor->frame_header_used += (size_t)n;
        continue;
      }
      if (n == 0) {
        errno = EPIPE;
        return NULL;
      }
      return NULL;
    }
    unsigned char *h = udev_monitor->frame_header;
    size_t payload_len = (size_t)h[8] | ((size_t)h[9] << 8) |
                         ((size_t)h[10] << 16) | ((size_t)h[11] << 24);
    if (memcmp(h, "UDEV", 4) != 0 || h[4] != 1 || h[5] != 0 || h[6] != 0 ||
        h[7] != 0 || payload_len == 0 || payload_len > UDEV_FRAME_PAYLOAD_MAX) {
      fprintf(stderr, "udev-shim MON_FRAME errno=%d fatal=1\n", EPROTO);
      close(udev_monitor->pipe_fd);
      udev_monitor->pipe_fd = -1;
      errno = EPROTO;
      return NULL;
    }
    udev_monitor->frame_payload_len = payload_len;
    while (udev_monitor->frame_payload_used < payload_len) {
      ssize_t n =
          read(udev_monitor->pipe_fd,
               udev_monitor->frame_payload + udev_monitor->frame_payload_used,
               payload_len - udev_monitor->frame_payload_used);
      if (n > 0) {
        udev_monitor->frame_payload_used += (size_t)n;
        continue;
      }
      if (n == 0) {
        errno = EPIPE;
        return NULL;
      }
      return NULL;
    }

    unsigned char payload[UDEV_FRAME_PAYLOAD_MAX];
    memcpy(payload, udev_monitor->frame_payload, payload_len);
    udev_monitor->frame_header_used = 0;
    udev_monitor->frame_payload_used = 0;
    udev_monitor->frame_payload_len = 0;
    if (payload[payload_len - 1] != '\0') {
      if (++udev_monitor->protocol_errors >= 3) {
        close(udev_monitor->pipe_fd);
        udev_monitor->pipe_fd = -1;
        errno = EPROTO;
        return NULL;
      }
      continue;
    }

    struct udev_device *d = calloc(1, sizeof(*d));
    if (!d)
      return NULL;
    d->refcount = 1;
    d->udev = udev_ref(udev_monitor->udev);
    d->initialized = 1;
    d->properties_loaded = 1;
    int malformed = 0;
    size_t offset = 0;
    while (offset < payload_len) {
      char *segment = (char *)payload + offset;
      size_t remaining = payload_len - offset;
      char *nul = memchr(segment, '\0', remaining);
      if (!nul) {
        malformed = 1;
        break;
      }
      size_t segment_len = (size_t)(nul - segment);
      offset += segment_len + 1;
      if (segment_len == 0)
        continue;
      char *eq = memchr(segment, '=', segment_len);
      if (!eq)
        continue;
      *eq = '\0';
      const char *key = segment;
      const char *value = eq + 1;
      size_t key_len = strlen(key);
      size_t value_len = strlen(value);
#define SET_ONCE(field)                                                        \
  do {                                                                         \
    if (d->field[0] || value_len >= sizeof(d->field))                          \
      malformed = 1;                                                           \
    else                                                                       \
      memcpy(d->field, value, value_len + 1);                                  \
  } while (0)
      if (strcmp(key, "ACTION") == 0)
        SET_ONCE(action);
      else if (strcmp(key, "DEVNAME") == 0)
        SET_ONCE(devnode);
      else if (strcmp(key, "DEVPATH") == 0)
        SET_ONCE(syspath);
      else if (strcmp(key, "SUBSYSTEM") == 0)
        SET_ONCE(subsystem);
      else if (strcmp(key, "DEVTYPE") == 0)
        SET_ONCE(devtype);
      else if (strcmp(key, "DEVNUM") == 0) {
        char *endptr = NULL;
        unsigned long value_num = strtoul(value, &endptr, 10);
        if (d->devnum || !*value || !endptr || *endptr || value_num == 0)
          malformed = 1;
        else
          d->devnum = (dev_t)value_num;
      } else if (key_len > 0 && key_len < UDEV_PROP_KEYLEN &&
                 value_len < UDEV_PROP_VALLEN &&
                 d->nprops < UDEV_DEV_PROPS_MAX) {
        for (int i = 0; i < d->nprops; i++)
          if (strcmp(d->props[i].key, key) == 0)
            malformed = 1;
        if (!malformed) {
          memcpy(d->props[d->nprops].key, key, key_len + 1);
          memcpy(d->props[d->nprops].value, value, value_len + 1);
          d->nprops++;
        }
      } else {
        malformed = 1;
      }
#undef SET_ONCE
    }
    if (!d->action[0] || !d->devnode[0] || !d->subsystem[0] || !d->devnum)
      malformed = 1;
    const char *slash = strrchr(d->devnode, '/');
    const char *sysname = slash ? slash + 1 : d->devnode;
    if (!malformed && valid_sysname(d->subsystem, sysname)) {
      snprintf(d->sysname, sizeof(d->sysname), "%s", sysname);
      snprintf(d->syspath, sizeof(d->syspath), "/sys/class/%s/%s", d->subsystem,
               d->sysname);
    } else {
      malformed = 1;
    }
    if (malformed) {
      udev_device_unref(d);
      if (++udev_monitor->protocol_errors >= 3) {
        close(udev_monitor->pipe_fd);
        udev_monitor->pipe_fd = -1;
        errno = EPROTO;
        return NULL;
      }
      continue;
    }
    udev_monitor->protocol_errors = 0;
    if ((udev_monitor->subsystem_filter[0] &&
         strcmp(udev_monitor->subsystem_filter, d->subsystem) != 0) ||
        (udev_monitor->devtype_filter[0] &&
         strcmp(udev_monitor->devtype_filter, d->devtype) != 0)) {
      udev_device_unref(d);
      continue;
    }
    return d;
  }
  errno = EAGAIN;
  return NULL;
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
  if (!udev) {
    errno = EINVAL;
    return NULL;
  }
  struct udev_enumerate *e = calloc(1, sizeof(struct udev_enumerate));
  if (e) {
    e->refcount = 1;
    e->udev = udev_ref(udev);
  }
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
  udev_unref(udev_enumerate->udev);
  free(udev_enumerate);
}

int udev_enumerate_add_match_subsystem(struct udev_enumerate *udev_enumerate,
                                       const char *subsystem) {
  if (!udev_enumerate || !subsystem)
    return -EINVAL;
  if (!*subsystem ||
      strlen(subsystem) >= sizeof(udev_enumerate->subsystem_filter))
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
  if (!*sysname || strlen(sysname) >= sizeof(udev_enumerate->sysname_filter))
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

  const char *subsystems[] = {"input", "drm", NULL};
  const char *only_subsys = udev_enumerate->subsystem_filter[0]
                                ? udev_enumerate->subsystem_filter
                                : NULL;

  if (only_subsys && strcmp(only_subsys, "input") != 0 &&
      strcmp(only_subsys, "drm") != 0) {
    struct udev_list_entry *old = udev_enumerate->devices;
    udev_enumerate->devices = NULL;
    while (old) {
      struct udev_list_entry *next = old->next;
      free(old);
      old = next;
    }
    return 0;
  }

  struct udev_list_entry *cur = udev_enumerate->devices;
  while (cur) {
    struct udev_list_entry *next = cur->next;
    free(cur);
    cur = next;
  }
  udev_enumerate->devices = NULL;
  char paths[MAX_UDEV_DEVICES][128];
  int count = 0;

  for (int si = 0; subsystems[si]; si++) {
    if (only_subsys && strcmp(subsystems[si], only_subsys) != 0)
      continue;

    char class_path[64];
    snprintf(class_path, sizeof(class_path), "/sys/class/%s", subsystems[si]);
    DIR *dir = opendir(class_path);
    if (!dir && only_subsys)
      return -errno;
    if (!dir)
      continue;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (entry->d_name[0] == '.')
        continue;
      if (udev_enumerate->sysname_filter[0] &&
          !match_pattern(udev_enumerate->sysname_filter, entry->d_name))
        continue;
      if (count == MAX_UDEV_DEVICES) {
        closedir(dir);
        return -E2BIG;
      }
      int n = snprintf(paths[count], sizeof(paths[count]), "/sys/class/%s/%s",
                       subsystems[si], entry->d_name);
      if (n < 0 || (size_t)n >= sizeof(paths[count]))
        continue;
      count++;
    }
    closedir(dir);
  }

  for (int i = 0; i < count; i++) {
    for (int j = i + 1; j < count; j++) {
      const char *a = strrchr(paths[i], '/') + 1;
      const char *b = strrchr(paths[j], '/') + 1;
      int cmp;
      if (strncmp(a, "card", 4) == 0 && all_digits(a + 4) &&
          strncmp(b, "card", 4) == 0 && all_digits(b + 4)) {
        unsigned long an = strtoul(a + 4, NULL, 10);
        unsigned long bn = strtoul(b + 4, NULL, 10);
        cmp = an == bn ? 0 : (an < bn ? -1 : 1);
      } else {
        cmp = strcmp(paths[i], paths[j]);
      }
      if (cmp > 0) {
        char tmp[128];
        memcpy(tmp, paths[i], sizeof(tmp));
        memcpy(paths[i], paths[j], sizeof(tmp));
        memcpy(paths[j], tmp, sizeof(tmp));
      }
    }
  }
  struct udev_list_entry **tail = &udev_enumerate->devices;
  for (int i = 0; i < count; i++) {
    struct udev_list_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
      struct udev_list_entry *allocated = udev_enumerate->devices;
      udev_enumerate->devices = NULL;
      while (allocated) {
        struct udev_list_entry *next = allocated->next;
        free(allocated);
        allocated = next;
      }
      errno = ENOMEM;
      return -ENOMEM;
    }
    snprintf(entry->name, sizeof(entry->name), "%s", paths[i]);
    *tail = entry;
    tail = &entry->next;
  }

  fprintf(stderr, "udev-shim enumerate subsystem=%s pattern=%s count=%d\n",
          only_subsys ? only_subsys : "*",
          udev_enumerate->sysname_filter[0] ? udev_enumerate->sysname_filter
                                            : "*",
          count);

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
