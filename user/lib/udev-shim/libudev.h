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

#define UDEV_DEV_PROPS_MAX 32
#define UDEV_PROP_KEYLEN 32
#define UDEV_PROP_VALLEN 64

struct udev_device {
  int refcount;
  char devnode[256];
  char syspath[256];
  char sysname[64];
  char subsystem[32];
  char devtype[32];
  char
      action[16]; /* monitor device 的 ACTION(add/remove/change);直读设备为空 */
  dev_t devnum;
  int initialized;
  /* property 表(monitor device 从 pipe KV 收到的 ID_INPUT_*、ID_SEAT 等)。
   * 对齐 Linux libudev 的 properties hashmap——monitor 路径 property 随 uevent
   * KV 到达 client,存 device 内存,get_property_value 查此表(不查 db)。
   * 数组代替 hashmap:freestanding user 态无现成 hashmap。32 槽覆盖
   * ID_INPUT*(12) + ID_SEAT + 少数 WL_*、MOUSE_DPI 等。nprops=0 表示无 property
   * (直读 device,走 db fallback)。 */
  struct {
    char key[UDEV_PROP_KEYLEN];
    char value[UDEV_PROP_VALLEN];
  } props[UDEV_DEV_PROPS_MAX];
  int nprops;
};

struct udev_list_entry {
  char name[128];
  char value[256];
  struct udev_list_entry *next;
};

struct udev_monitor {
  struct udev *udev; /* udev_ref 持有,unref 时释放 */
  int sock_fd; /* AF_UNIX conn fd(connect 后短暂持有,拿 pipe fd 后关) */
  int pipe_fd; /* SCM_RIGHTS 收到的 pipe rd fd(get_fd 返此,可 epoll) */
  int subscribed; /* enable_receiving 后置 1(幂等) */
};

struct udev_enumerate {
  int refcount;
  struct udev *udev;
  char subsystem_filter[32];       // "input" 等，空串表示不过滤
  char sysname_filter[64];         // "event*" 等，空串表示不过滤
  struct udev_list_entry *devices; // scan 结果链表头
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

/* 对齐 Linux libudev：udev-seat.c:172 用此宏遍历 enumerate 结果。
 * get_next 已由 shim 提供（声明见上 / 实现见 udev.c:437），此处仅补 foreach
 * 宏。 */
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
struct udev *udev_device_get_udev(struct udev_device *udev_device);

char *udev_device_get_property_value_w(char *property, size_t property_size,
                                       struct udev_device *udev_device,
                                       const char *key);

#ifdef __cplusplus
}
#endif

#endif
