/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "libudev.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xos/errno.h"
#include "xos/fcntl.h"

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xos/input.h>
#include <xos/ioctl.h>

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

// 尾部 '*' 通配符匹配（libinput 常用
// udev_enumerate_add_match_sysname("event*")）。 仅支持形如 "event*"
// 的尾部通配；无 '*' 时退化为精确 strcmp。 返回 1 匹配 / 0 不匹配（user 态无
// stdbool.h，用 int）。
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
  if (fd < 0)
    return 0;
  unsigned long bits[NBITS(EV_MAX + 1)];
  memset(bits, 0, sizeof(bits));
  int rc = ioctl(fd, EVIOCGBIT(0, sizeof(bits)), bits);
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

  // subsystem 固定 "input"（scan_devices 仅扫描 /dev/input），先填以便 syspath
  // 构造引用。
  strncpy(d->subsystem, "input", sizeof(d->subsystem) - 1);
  d->subsystem[sizeof(d->subsystem) - 1] = '\0';

  // Build syspath: /sys/class/<subsystem>/<sysname>（与内核 sysfs
  // 实际挂载路径一致）
  const char *basename = strrchr(devnode, '/');
  if (!basename)
    basename = devnode;
  else
    basename++;

  // subsystem 此时已固定为 "input"；保留分支结构以便后续
  // 扩展到 drm 等其它子系统。
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

/* udev.c — udev_device_get_property_value 乙落地(user 态 C,int 代 bool) */
const char *udev_device_get_property_value(struct udev_device *udev_device,
                                           const char *key) {
  if (!udev_device || !key)
    return NULL;

  /* 直读 db 文件(对齐 Linux libudev 直读 /run/udev/data/<key>) */
  char key_str[32], path[80];
  snprintf(key_str, sizeof(key_str), "%u", (unsigned)udev_device->devnum);
  snprintf(path, sizeof(path), "/run/udev/data/%s", key_str);

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL; /* db 文件不存在(udevd 未起/未写过)→ 返 NULL(降级,§5.2) */

  static char
      db_buf[2048]; /* 单次调用有效,调用者需立即拷贝(对齐 Linux libudev) */
  ssize_t n = read(fd, db_buf, sizeof(db_buf) - 1);
  close(fd);
  if (n <= 0)
    return NULL;
  db_buf[n] = '\0';

  /* 解析 KEY=VALUE\n 找指定 key */
  char *line = db_buf;
  while (line && *line) {
    char *eol = strchr(line, '\n');
    if (eol)
      *eol = '\0';
    char *eq = strchr(line, '=');
    if (eq) {
      *eq = '\0';
      if (strcmp(line, key) == 0) {
        return eq + 1; /* 返指向 db_buf 内的指针,调用者需立即拷贝 */
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
  (void)udev_device;
  return NULL;
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

  // syspath 已为 "/sys/class/input/event0"（P1 修复后）。
  // evdev 布局：name 在设备目录根，bustype/vendor/product/version 在 id/ 子目录
  // （见 devtmpfs.c:661 target = (i==0) ? devdir : iddir）。
  // drm 等其它子系统属性在类目录根。
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

  // 单次调用有效的静态缓冲区，下次调用覆盖（与 Linux libudev 行为一致，
  // 调用者需立即拷贝）。
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

// ======================== monitor (no-op stubs) ========================

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev,
                                                   const char *name) {
  (void)udev;
  (void)name;
  struct udev_monitor *m = calloc(1, sizeof(struct udev_monitor));
  if (m)
    m->fd = -1;
  return m;
}

int udev_monitor_filter_add_match_subsystem_devtype(
    struct udev_monitor *udev_monitor, const char *subsystem,
    const char *devtype) {
  (void)udev_monitor;
  (void)subsystem;
  (void)devtype;
  return 0;
}

int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor) {
  (void)udev_monitor;
  return 0;
}

int udev_monitor_get_fd(struct udev_monitor *udev_monitor) {
  return udev_monitor ? udev_monitor->fd : -1;
}

struct udev_device *
udev_monitor_receive_device(struct udev_monitor *udev_monitor) {
  (void)udev_monitor;
  return NULL;
}

void udev_monitor_unref(struct udev_monitor *udev_monitor) {
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

  // 已知子系统白名单。无 subsystem_filter 时扫描全部。
  const char *subsystems[] = {"input", "drm", NULL};
  const char *only_subsys = udev_enumerate->subsystem_filter[0]
                                ? udev_enumerate->subsystem_filter
                                : NULL;

  // 清除旧结果（支持重复 scan）。
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
