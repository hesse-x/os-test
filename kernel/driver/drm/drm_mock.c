/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/drm/drm_core.h"

#ifdef TEST

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xos/errno.h>

#include "arch/x64/utils.h"
#include "drm/drm.h"
#include "drm/drm_mode.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/bsd_types.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/kasan.h"

struct drm_core_device;
struct xtask;

struct drm_mock_private {
  unsigned generation;
};

static struct drm_core_device *drm_mock_device;
static struct drm_core_device *drm_mock_device2;
static struct drm_mock_private drm_mock_generations[2] = {
    {.generation = 1},
    {.generation = 2},
};

static long drm_mock_version(struct file *file, void *arg) {
  struct drm_version *version = arg;
  char name[32];
  if (!version)
    return -EFAULT;

  struct drm_core_device *dev = drm_core_file_device(file);
  struct drm_mock_private *private = drm_core_driver_private(dev);
  if (!private)
    return -ENODEV;
  snprintf(name, sizeof(name), "xos_drm_mock%u", private->generation);

  version->version_major = 1;
  version->version_minor = 0;
  version->version_patchlevel = 0;
  if (version->name && version->name_len) {
    size_t copied = __strlen(name);
    if (copied >= version->name_len)
      copied = version->name_len - 1;
    if (copy_to_user(version->name, name, copied))
      return -EFAULT;
    char nul = '\0';
    if (copy_to_user(version->name + copied, &nul, 1))
      return -EFAULT;
  }
  version->name_len = __strlen(name);
  version->date_len = 0;
  version->desc_len = 0;
  return 0;
}

static long drm_mock_get_cap(void *arg) {
  struct drm_get_cap *cap = arg;
  if (!cap)
    return -EFAULT;
  switch (cap->capability) {
  case DRM_CAP_DUMB_BUFFER:
    cap->value = 0;
    return 0;
  case DRM_CAP_TIMESTAMP_MONOTONIC:
    cap->value = 1;
    return 0;
  case DRM_CAP_PRIME:
    cap->value = 0;
    return 0;
  default:
    cap->value = 0;
    return -EINVAL;
  }
}

static long drm_mock_ioctl(struct xtask *proc, struct file *file, uint32_t cmd,
                           void *arg) {
  (void)proc;
  if (cmd == DRM_IOCTL_VERSION)
    return drm_mock_version(file, arg);
  if (cmd == DRM_IOCTL_GET_CAP)
    return drm_mock_get_cap(arg);
  return -ENOTTY;
}

static const struct dev_ops drm_mock_primary_ops = {
    .driver_pid = 0,
    .is_block = false,
    .ioctl_file = drm_mock_ioctl,
};

static const struct dev_ops drm_mock_render_ops = {
    .driver_pid = 0,
    .is_block = false,
    .ioctl_file = drm_mock_ioctl,
};

static struct drm_core_device *drm_mock_alloc(unsigned generation) {
  struct drm_core_config config = {
      .driver_name = "xos_drm_mock",
      .subsystem_target = "/sys/class/drm",
      .primary_ops = &drm_mock_primary_ops,
      .render_ops = &drm_mock_render_ops,
      .driver_private = &drm_mock_generations[generation - 1],
  };
  return drm_core_device_alloc(&config);
}

static uint32_t drm_mock_init_kms(struct drm_core_device *dev,
                                  uint64_t generation) {
  uint32_t object = drm_core_object_create(dev, DRM_MODE_OBJECT_CONNECTOR);
  uint32_t value_property =
      drm_core_property_create_range(dev, "MOCK_VALUE", 0, 0xffff, false);
  uint32_t blob_property =
      drm_core_property_create_blob(dev, "MOCK_BLOB", true);
  uint32_t blob = drm_core_blob_create(dev, &generation, sizeof(generation));
  BUG_ON(!object || !value_property || !blob_property || !blob);
  BUG_ON(drm_core_object_add_property(dev, object, DRM_MODE_OBJECT_CONNECTOR,
                                      value_property, generation));
  BUG_ON(drm_core_object_add_property(dev, object, DRM_MODE_OBJECT_CONNECTOR,
                                      blob_property, blob));
  return object;
}

void drm_mock_register_test_device(void) {
  struct drm_core_device *failed = drm_mock_alloc(1);
  BUG_ON(!failed);
  drm_core_test_fail_once(DRM_CORE_FAULT_RENDER_PUBLISH);
  int rc = drm_core_device_register(failed, DRM_NODE_PRIMARY | DRM_NODE_RENDER);
  BUG_ON(rc != -ENOMEM);
  BUG_ON(drm_core_device_state(failed) != DRM_CORE_INITIALIZED);
  drm_core_device_put(failed);

  drm_mock_device = drm_mock_alloc(2);
  BUG_ON(!drm_mock_device);
  rc = drm_core_device_register(drm_mock_device,
                                DRM_NODE_PRIMARY | DRM_NODE_RENDER);
  BUG_ON(rc != 0);
  int mock_slot = drm_core_device_slot(drm_mock_device);
  BUG_ON(mock_slot < 0);
  BUG_ON(drm_core_object_alloc(drm_mock_device) != 1);
  BUG_ON(drm_core_object_alloc(drm_mock_device) != 2);
  uint32_t mock_object = drm_mock_init_kms(drm_mock_device, 2);
  BUG_ON(mock_object != 3);

  char path[32];
  snprintf(path, sizeof(path), "dri/card%d", mock_slot);
  struct inode *new_inode = devtmpfs_lookup(path);
  BUG_ON(!new_inode);
  struct file new_file = {.inode = new_inode};
  struct dev_ops *new_ops = dev_ops_peek_by_inode(new_inode);
  BUG_ON(!new_ops);
  BUG_ON(new_ops->open_file(NULL, &new_file) != 0);
  struct drm_mock_private *new_private =
      drm_core_driver_private(drm_core_file_device(&new_file));
  BUG_ON(!new_private || new_private->generation != 2);
  struct file second_file = {.inode = new_inode};
  BUG_ON(new_ops->open_file(NULL, &second_file) != 0);
  BUG_ON(drm_core_event_queue(&new_file, 0x11, 3, 0));
  BUG_ON(drm_core_event_queue(&second_file, 0x22, 3, 0));
  drm_core_event_tick(drm_mock_device, 1);
  BUG_ON(!new_ops->poll_file(NULL, &new_file, 0));
  BUG_ON(!new_ops->poll_file(NULL, &second_file, 0));
  BUG_ON(new_ops->close_file(NULL, &second_file) != 0);
  BUG_ON(new_ops->close_file(NULL, &new_file) != 0);
  inode_put(new_inode);

  struct drm_get_cap cap = {.capability = DRM_CAP_TIMESTAMP_MONOTONIC};
  BUG_ON(drm_mock_get_cap(&cap) != 0 || cap.value != 1);

  // With no hardware DRM device, retain a second mock so pure-core boots
  // exercise two simultaneously registered devices as well.
  if (mock_slot == 0) {
    drm_mock_device2 = drm_mock_alloc(1);
    BUG_ON(!drm_mock_device2);
    rc = drm_core_device_register(drm_mock_device2,
                                  DRM_NODE_PRIMARY | DRM_NODE_RENDER);
    BUG_ON(rc != 0 || drm_core_device_slot(drm_mock_device2) != 1);
    BUG_ON(drm_core_object_alloc(drm_mock_device2) != 1);
    uint32_t second_object = drm_mock_init_kms(drm_mock_device2, 1);
    uint32_t property_id = 0;
    uint64_t value = 0;
    BUG_ON(second_object != 2);
    BUG_ON(drm_core_object_property_by_name(
        drm_mock_device2, second_object, DRM_MODE_OBJECT_CONNECTOR,
        "MOCK_VALUE", &property_id, &value));
    BUG_ON(property_id != 1 || value != 1);
    BUG_ON(drm_core_object_property_by_name(
        drm_mock_device, mock_object, DRM_MODE_OBJECT_CONNECTOR, "MOCK_VALUE",
        &property_id, &value));
    BUG_ON(property_id != 1 || value != 2);
  }

  printk(LOG_INFO,
         "drm lifecycle selftest: PASS (register rollback, slot=%d, "
         "devices=%d)\n",
         mock_slot, mock_slot == 0 ? 2 : 1);
}

#endif
