/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/drm/drm_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <xos/errno.h>
#include <xos/mman.h>
#include <xos/page.h>
#include <xos/socket.h>

#include "arch/x64/apic.h"
#include "arch/x64/utils.h"
#include "drm/drm.h"
#include "drm/drm_mode.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/kfcntl.h" // IWYU pragma: keep
#include "kernel/bsd/poll_types.h"
#include "kernel/bsd/sysfs.h"
#include "kernel/driver/bsd_types.h"
#include "kernel/driver/dma_buf.h"
#include "kernel/driver/dma_resv.h"
#include "kernel/driver/drm/drm_fence.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/mutex.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"
#include "utils/macro.h"

#define DRM_CORE_MAJOR 226
#define DRM_CORE_SLOTS 64
#define DRM_CORE_MAX_OBJECTS 64
#define DRM_CORE_MAX_PROPERTIES 64
#define DRM_CORE_MAX_OBJECT_PROPERTIES 16
#define DRM_CORE_MAX_PROPERTY_ENUMS 16
#define DRM_CORE_MAX_BLOBS 32
#define DRM_CORE_MAX_HANDLES 256
#define DRM_CORE_MAX_SYNCOBJS 256
#define DRM_CORE_MMAP_OFFSET_START 0x100000ULL

int bsd_drm_prime_fd_install(xtask *proc, struct drm_prime_object *object,
                             bool cloexec);
struct file *bsd_drm_prime_fd_get(xtask *proc, int fd);
struct file *bsd_sync_file_fd_get(xtask *proc, int fd);

struct drm_gem_object {
  refcount_t refcount;
  spinlock reservation_lock;
  struct drm_fence *exclusive_fence;
  struct drm_core_device *dev;
  uint64_t size;
  uint64_t mmap_offset;
  struct page **pages;
  uint32_t page_count;
  void *private;
  const struct drm_gem_object_ops *ops;
  struct dma_buf *dmabuf;
};

struct drm_core_handle {
  uint32_t handle;
  struct drm_gem_object *object;
};

struct drm_core_syncobj {
  uint32_t handle;
  struct drm_fence *fence;
};

enum drm_core_property_type {
  DRM_CORE_PROPERTY_RANGE,
  DRM_CORE_PROPERTY_ENUM,
  DRM_CORE_PROPERTY_BLOB,
  DRM_CORE_PROPERTY_OBJECT,
};

struct drm_core_property_enum {
  uint64_t value;
  char name[DRM_CORE_PROP_NAME_LEN];
};

struct drm_core_property {
  uint32_t id;
  enum drm_core_property_type type;
  bool immutable;
  char name[DRM_CORE_PROP_NAME_LEN];
  uint64_t min;
  uint64_t max;
  uint32_t object_type;
  size_t enum_count;
  struct drm_core_property_enum enums[DRM_CORE_MAX_PROPERTY_ENUMS];
};

struct drm_core_object {
  uint32_t id;
  uint32_t type;
  size_t property_count;
  uint32_t property_ids[DRM_CORE_MAX_OBJECT_PROPERTIES];
  uint64_t property_values[DRM_CORE_MAX_OBJECT_PROPERTIES];
};

struct drm_core_blob {
  uint32_t id;
  size_t length;
  void *data;
};

struct drm_core_minor {
  struct drm_core_device *dev;
  uint32_t type;
  unsigned minor_id;
  bool published;
  char dev_name[32];
  char sysfs_name[16];
  struct dev_ops ops;
  struct sysfs_node *class_node;
  struct sysfs_node *devchar_node;
  struct sysfs_attr driver_attr;
  struct sysfs_attr dev_attr;
};

struct drm_core_device {
  refcount_t refcount;
  enum drm_core_state state;
  int slot;
  uint32_t node_mask;
  char driver_name[32];
  char subsystem_target[32];
  const struct dev_ops *primary_template;
  const struct dev_ops *render_template;
  void *driver_private;
  void (*master_drop)(void *driver_private);
  void (*driver_release)(void *driver_private);
  mutex file_mutex;
  list_node files;
  struct drm_core_file *master;
  uint32_t next_magic;
  uint32_t next_object_id;
  uint32_t next_property_id;
  uint32_t next_blob_id;
  uint64_t next_mmap_page;
  struct drm_core_object objects[DRM_CORE_MAX_OBJECTS];
  struct drm_core_property properties[DRM_CORE_MAX_PROPERTIES];
  struct drm_core_blob blobs[DRM_CORE_MAX_BLOBS];
  struct drm_core_minor primary;
  struct drm_core_minor render;
};

struct drm_core_file {
  list_node node;
  struct drm_core_device *dev;
  struct file *file;
  bool render;
  bool master;
  bool authenticated;
  uint32_t magic;
  uint64_t client_caps;
  spinlock event_lock;
  bool event_armed;
  bool event_pending;
  uint64_t event_deadline_ns;
  uint64_t event_user_data;
  uint32_t event_crtc_id;
  uint32_t event_sequence;
  wait_queue_head event_wq;
  struct drm_core_handle *handles;
  uint32_t next_handle;
  struct drm_core_syncobj syncobjs[DRM_CORE_MAX_SYNCOBJS];
  uint32_t next_syncobj_handle;
};

static mutex drm_registry_mutex;
static bool drm_slots[DRM_CORE_SLOTS];
static struct sysfs_node *drm_class_dir;
#ifdef TEST
static enum drm_core_fault_point drm_fault_once;
#endif

void drm_core_init(void) {
  mutex_init(&drm_registry_mutex);
  __memset(drm_slots, 0, sizeof(drm_slots));
  drm_class_dir = sysfs_class_dir("drm");
}

#ifdef TEST
void drm_core_test_fail_once(enum drm_core_fault_point point) {
  drm_fault_once = point;
}

static bool drm_core_fault(enum drm_core_fault_point point) {
  if (drm_fault_once != point)
    return false;
  drm_fault_once = DRM_CORE_FAULT_NONE;
  return true;
}
#endif

void drm_core_device_get(struct drm_core_device *dev) {
  if (dev)
    refcount_inc(&dev->refcount);
}

void drm_core_device_put(struct drm_core_device *dev) {
  if (!dev)
    return;
  if (!refcount_dec_and_test(&dev->refcount))
    return;
  ASSERT(dev->state != DRM_CORE_REGISTERED && !dev->primary.published &&
         !dev->render.published);
  for (size_t i = 0; i < DRM_CORE_MAX_BLOBS; i++)
    if (dev->blobs[i].data)
      kfree(dev->blobs[i].data);
  if (dev->driver_release)
    dev->driver_release(dev->driver_private);
  kfree(dev);
}

static void drm_minor_ops_release(struct dev_ops *ops) {
  struct drm_core_minor *minor = ops ? ops->instance_priv : NULL;
  ASSERT(minor && minor->dev);
  drm_core_device_put(minor->dev);
}

static void drm_owner_get(void *owner) {
  drm_core_device_get((struct drm_core_device *)owner);
}

static void drm_owner_put(void *owner) {
  drm_core_device_put((struct drm_core_device *)owner);
}

static const struct vma_owner_ops drm_owner_ops = {
    .get = drm_owner_get,
    .put = drm_owner_put,
};

static ssize_t drm_minor_driver_show(char *buf, size_t len, void *priv) {
  struct drm_core_minor *minor = priv;
  if (!minor || !minor->dev)
    return -ENODEV;
  return snprintf(buf, len, "%s\n", minor->dev->driver_name);
}

static ssize_t drm_minor_dev_show(char *buf, size_t len, void *priv) {
  struct drm_core_minor *minor = priv;
  if (!minor)
    return -ENODEV;
  return snprintf(buf, len, "%u:%u\n", DRM_CORE_MAJOR, minor->minor_id);
}

static struct drm_core_minor *drm_minor_for_type(struct drm_core_device *dev,
                                                 uint32_t type) {
  if (!dev)
    return NULL;
  if (type == DRM_NODE_PRIMARY)
    return &dev->primary;
  if (type == DRM_NODE_RENDER)
    return &dev->render;
  return NULL;
}

static struct drm_core_minor *drm_minor_from_file(struct file *file) {
  if (!file || !file->inode)
    return NULL;
  struct dev_ops *ops = dev_ops_peek_by_inode(file->inode);
  return ops ? ops->instance_priv : NULL;
}

static struct drm_core_file *drm_file_find_locked(struct drm_core_device *dev,
                                                  struct file *file) {
  for (list_node *node = dev->files.next; node != &dev->files;
       node = node->next) {
    struct drm_core_file *df = LIST_ENTRY(node, struct drm_core_file, node);
    if (df->file == file)
      return df;
  }
  return NULL;
}

static struct drm_core_file *drm_file_find(struct file *file) {
  struct drm_core_device *dev = drm_core_file_device(file);
  if (!dev)
    return NULL;
  mutex_lock(&dev->file_mutex);
  struct drm_core_file *df = drm_file_find_locked(dev, file);
  mutex_unlock(&dev->file_mutex);
  return df;
}

static int drm_minor_open_file(xtask *proc, struct file *file) {
  struct drm_core_minor *minor = drm_minor_from_file(file);
  if (!minor || !minor->dev)
    return -ENODEV;

  // This is the open/unregister linearization point. An open that observes
  // REGISTERED owns a dev_ops fd reference and remains tied to this generation.
  if (__atomic_load_n(&minor->dev->state, __ATOMIC_ACQUIRE) !=
      DRM_CORE_REGISTERED)
    return -ENODEV;
  if (minor->type == DRM_NODE_PRIMARY &&
      !(minor->dev->node_mask & DRM_NODE_PRIMARY))
    return -ENODEV;
  if (minor->type == DRM_NODE_RENDER &&
      !(minor->dev->node_mask & DRM_NODE_RENDER))
    return -ENODEV;

  const struct dev_ops *backend = minor->type == DRM_NODE_PRIMARY
                                      ? minor->dev->primary_template
                                      : minor->dev->render_template;
  if (!backend)
    return -ENODEV;
  struct drm_core_file *df = kmalloc(sizeof(*df));
  if (!df)
    return -ENOMEM;
  __memset(df, 0, sizeof(*df));
  df->handles = kmalloc(sizeof(*df->handles) * DRM_CORE_MAX_HANDLES);
  if (!df->handles) {
    kfree(df);
    return -ENOMEM;
  }
  __memset(df->handles, 0, sizeof(*df->handles) * DRM_CORE_MAX_HANDLES);
  list_init(&df->node);
  df->dev = minor->dev;
  df->file = file;
  df->render = minor->type == DRM_NODE_RENDER;
  df->event_lock = (spinlock)SPINLOCK_INIT;
  init_wait_queue_head(&df->event_wq);
  drm_core_device_get(df->dev);
  mutex_lock(&df->dev->file_mutex);
  list_push_back(&df->dev->files, &df->node);
  mutex_unlock(&df->dev->file_mutex);

  int rc = 0;
  if (backend->open_file)
    rc = backend->open_file(proc, file);
  // Dynamic DRM nodes require OFD-aware opens; there is no reliable fd number
  // available here with which to invoke a legacy open callback.
  else if (backend->open)
    rc = -ENOSYS;
  if (rc) {
    mutex_lock(&df->dev->file_mutex);
    list_remove(&df->node);
    mutex_unlock(&df->dev->file_mutex);
    drm_core_device_put(df->dev);
    kfree(df->handles);
    kfree(df);
  }
  return rc;
}

static int drm_minor_close_file(xtask *proc, struct file *file) {
  struct drm_core_minor *minor = drm_minor_from_file(file);
  if (!minor || !minor->dev)
    return 0;
  const struct dev_ops *backend = minor->type == DRM_NODE_PRIMARY
                                      ? minor->dev->primary_template
                                      : minor->dev->render_template;
  int rc = backend && backend->close_file ? backend->close_file(proc, file) : 0;

  mutex_lock(&minor->dev->file_mutex);
  struct drm_core_file *df = drm_file_find_locked(minor->dev, file);
  bool dropped_master = df && minor->dev->master == df;
  if (dropped_master) {
    minor->dev->master = NULL;
    df->master = false;
  }
  if (df) {
    spin_lock(&df->event_lock);
    df->event_armed = false;
    df->event_pending = false;
    spin_unlock(&df->event_lock);
    list_remove(&df->node);
    for (size_t i = 0; i < DRM_CORE_MAX_HANDLES; i++) {
      df->handles[i].handle = 0;
    }
    for (size_t i = 0; i < DRM_CORE_MAX_SYNCOBJS; i++)
      df->syncobjs[i].handle = 0;
  }
  mutex_unlock(&minor->dev->file_mutex);
  if (dropped_master && minor->dev->master_drop)
    minor->dev->master_drop(minor->dev->driver_private);
  if (df) {
    for (size_t i = 0; i < DRM_CORE_MAX_HANDLES; i++) {
      struct drm_gem_object *object = df->handles[i].object;
      df->handles[i].object = NULL;
      if (object)
        drm_gem_object_put(object);
    }
    for (size_t i = 0; i < DRM_CORE_MAX_SYNCOBJS; i++) {
      struct drm_fence *fence = df->syncobjs[i].fence;
      df->syncobjs[i].fence = NULL;
      drm_fence_put(fence);
    }
    drm_core_device_put(df->dev);
    kfree(df->handles);
    kfree(df);
  }
  return rc;
}

static struct drm_core_object *drm_object_find(struct drm_core_device *dev,
                                               uint32_t id, uint32_t type) {
  if (!dev || !id)
    return NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_OBJECTS; i++) {
    struct drm_core_object *object = &dev->objects[i];
    if (object->id == id && (!type || object->type == type))
      return object;
  }
  return NULL;
}

static struct drm_core_property *drm_property_find(struct drm_core_device *dev,
                                                   uint32_t id) {
  if (!dev || !id)
    return NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_PROPERTIES; i++)
    if (dev->properties[i].id == id)
      return &dev->properties[i];
  return NULL;
}

static struct drm_core_blob *drm_blob_find(struct drm_core_device *dev,
                                           uint32_t id) {
  if (!dev || !id)
    return NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_BLOBS; i++)
    if (dev->blobs[i].id == id)
      return &dev->blobs[i];
  return NULL;
}

static long drm_core_set_object_property(struct drm_core_device *dev,
                                         uint32_t object_id,
                                         uint32_t object_type,
                                         uint32_t property_id, uint64_t value) {
  mutex_lock(&dev->file_mutex);
  struct drm_core_object *object = drm_object_find(dev, object_id, object_type);
  struct drm_core_property *property = drm_property_find(dev, property_id);
  if (!object || !property) {
    mutex_unlock(&dev->file_mutex);
    return -ENOENT;
  }
  if (property->immutable) {
    mutex_unlock(&dev->file_mutex);
    return -EINVAL;
  }
  if (property->type == DRM_CORE_PROPERTY_RANGE &&
      (value < property->min || value > property->max)) {
    mutex_unlock(&dev->file_mutex);
    return -EINVAL;
  }
  if (property->type == DRM_CORE_PROPERTY_ENUM) {
    bool valid = false;
    for (size_t i = 0; i < property->enum_count; i++)
      valid |= property->enums[i].value == value;
    if (!valid) {
      mutex_unlock(&dev->file_mutex);
      return -EINVAL;
    }
  }
  if (property->type == DRM_CORE_PROPERTY_OBJECT && value &&
      !drm_object_find(dev, (uint32_t)value, property->object_type)) {
    mutex_unlock(&dev->file_mutex);
    return -ENOENT;
  }
  for (size_t i = 0; i < object->property_count; i++) {
    if (object->property_ids[i] == property_id) {
      object->property_values[i] = value;
      mutex_unlock(&dev->file_mutex);
      return 0;
    }
  }
  mutex_unlock(&dev->file_mutex);
  return -ENOENT;
}

static long drm_core_kms_ioctl(struct drm_core_file *df, uint32_t cmd,
                               void *arg, bool *handled) {
  *handled = true;
  switch (cmd) {
  case DRM_IOCTL_MODE_GETPROPERTY: {
    struct drm_mode_get_property *request = arg;
    if (!request)
      return -EFAULT;
    mutex_lock(&df->dev->file_mutex);
    struct drm_core_property *property =
        drm_property_find(df->dev, request->prop_id);
    if (!property) {
      mutex_unlock(&df->dev->file_mutex);
      return -ENOENT;
    }
    __memset(request->name, 0, sizeof(request->name));
    __strncpy(request->name, property->name, sizeof(request->name) - 1);
    request->flags = property->immutable ? DRM_MODE_PROP_IMMUTABLE : 0;
    request->count_values = 0;
    request->count_enum_blobs = 0;
    if (property->type == DRM_CORE_PROPERTY_RANGE) {
      uint64_t values[2] = {property->min, property->max};
      request->flags |= DRM_MODE_PROP_RANGE;
      request->count_values = 2;
      if (request->values_ptr &&
          copy_to_user((void *)(uintptr_t)request->values_ptr, values,
                       sizeof(values))) {
        mutex_unlock(&df->dev->file_mutex);
        return -EFAULT;
      }
    } else if (property->type == DRM_CORE_PROPERTY_ENUM) {
      request->flags |= DRM_MODE_PROP_ENUM;
      request->count_enum_blobs = property->enum_count;
      if (request->enum_blob_ptr) {
        for (size_t i = 0; i < property->enum_count; i++) {
          struct drm_mode_property_enum item;
          __memset(&item, 0, sizeof(item));
          item.value = property->enums[i].value;
          __strncpy(item.name, property->enums[i].name, sizeof(item.name) - 1);
          if (copy_to_user((void *)(uintptr_t)(request->enum_blob_ptr +
                                               i * sizeof(item)),
                           &item, sizeof(item))) {
            mutex_unlock(&df->dev->file_mutex);
            return -EFAULT;
          }
        }
      }
    } else if (property->type == DRM_CORE_PROPERTY_BLOB) {
      request->flags |= DRM_MODE_PROP_BLOB;
    } else {
      request->flags |= DRM_MODE_PROP_OBJECT;
    }
    mutex_unlock(&df->dev->file_mutex);
    return 0;
  }
  case DRM_IOCTL_MODE_GETPROPBLOB: {
    struct drm_mode_get_blob *request = arg;
    if (!request)
      return -EFAULT;
    mutex_lock(&df->dev->file_mutex);
    struct drm_core_blob *blob = drm_blob_find(df->dev, request->blob_id);
    if (!blob) {
      mutex_unlock(&df->dev->file_mutex);
      return -ENOENT;
    }
    uint32_t supplied = request->length;
    request->length = (uint32_t)blob->length;
    size_t copied = supplied < blob->length ? supplied : blob->length;
    if (request->data && copied &&
        copy_to_user((void *)(uintptr_t)request->data, blob->data, copied)) {
      mutex_unlock(&df->dev->file_mutex);
      return -EFAULT;
    }
    mutex_unlock(&df->dev->file_mutex);
    return 0;
  }
  case DRM_IOCTL_MODE_OBJ_GETPROPERTIES: {
    struct drm_mode_obj_get_properties *request = arg;
    if (!request)
      return -EFAULT;
    mutex_lock(&df->dev->file_mutex);
    struct drm_core_object *object =
        drm_object_find(df->dev, request->obj_id, request->obj_type);
    if (!object) {
      mutex_unlock(&df->dev->file_mutex);
      return -ENOENT;
    }
    request->count_props = object->property_count;
    if (request->props_ptr && request->prop_values_ptr) {
      if (copy_to_user((void *)(uintptr_t)request->props_ptr,
                       object->property_ids,
                       object->property_count * sizeof(uint32_t)) ||
          copy_to_user((void *)(uintptr_t)request->prop_values_ptr,
                       object->property_values,
                       object->property_count * sizeof(uint64_t))) {
        mutex_unlock(&df->dev->file_mutex);
        return -EFAULT;
      }
    }
    mutex_unlock(&df->dev->file_mutex);
    return 0;
  }
  case DRM_IOCTL_MODE_SETPROPERTY: {
    struct drm_mode_connector_set_property *request = arg;
    if (!request)
      return -EFAULT;
    return drm_core_set_object_property(df->dev, request->connector_id,
                                        DRM_MODE_OBJECT_CONNECTOR,
                                        request->prop_id, request->value);
  }
  case DRM_IOCTL_MODE_OBJ_SETPROPERTY: {
    struct drm_mode_obj_set_property *request = arg;
    if (!request)
      return -EFAULT;
    return drm_core_set_object_property(df->dev, request->obj_id,
                                        request->obj_type, request->prop_id,
                                        request->value);
  }
  default:
    *handled = false;
    return 0;
  }
}

static struct drm_core_syncobj *
drm_syncobj_find_locked(struct drm_core_file *df, uint32_t handle) {
  if (!handle)
    return NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_SYNCOBJS; i++)
    if (df->syncobjs[i].handle == handle)
      return &df->syncobjs[i];
  return NULL;
}

static int drm_syncobj_create(struct drm_core_file *df, bool signaled,
                              uint32_t *handle) {
  struct drm_fence *fence = drm_fence_create(signaled);
  if (!fence)
    return -ENOMEM;
  mutex_lock(&df->dev->file_mutex);
  struct drm_core_syncobj *entry = NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_SYNCOBJS; i++)
    if (!df->syncobjs[i].handle) {
      entry = &df->syncobjs[i];
      break;
    }
  if (!entry) {
    mutex_unlock(&df->dev->file_mutex);
    drm_fence_put(fence);
    return -ENOSPC;
  }
  uint32_t candidate = df->next_syncobj_handle;
  if (!candidate)
    candidate = 1;
  while (drm_syncobj_find_locked(df, candidate))
    if (++candidate == 0) {
      mutex_unlock(&df->dev->file_mutex);
      drm_fence_put(fence);
      return -ENOSPC;
    }
  entry->handle = candidate;
  entry->fence = fence;
  df->next_syncobj_handle = candidate + 1;
  *handle = candidate;
  mutex_unlock(&df->dev->file_mutex);
  return 0;
}

static int drm_syncobj_collect(struct drm_core_file *df, uint64_t user_handles,
                               uint32_t count, struct drm_fence ***fences_out) {
  if (!user_handles || !count || count > DRM_CORE_MAX_SYNCOBJS)
    return -EINVAL;
  uint32_t *handles = kmalloc(count * sizeof(*handles));
  struct drm_fence **fences = kmalloc(count * sizeof(*fences));
  if (!handles || !fences) {
    kfree(handles);
    kfree(fences);
    return -ENOMEM;
  }
  __memset(fences, 0, count * sizeof(*fences));
  if (copy_from_user(handles, (void *)(uintptr_t)user_handles,
                     count * sizeof(*handles))) {
    kfree(handles);
    kfree(fences);
    return -EFAULT;
  }
  int rc = 0;
  mutex_lock(&df->dev->file_mutex);
  for (uint32_t i = 0; i < count; i++) {
    struct drm_core_syncobj *entry = drm_syncobj_find_locked(df, handles[i]);
    if (!entry) {
      rc = -ENOENT;
      break;
    }
    fences[i] = entry->fence;
    drm_fence_get(fences[i]);
  }
  mutex_unlock(&df->dev->file_mutex);
  kfree(handles);
  if (rc) {
    for (uint32_t i = 0; i < count; i++)
      drm_fence_put(fences[i]);
    kfree(fences);
    return rc;
  }
  *fences_out = fences;
  return 0;
}

static void drm_syncobj_put_fences(struct drm_fence **fences, uint32_t count) {
  for (uint32_t i = 0; i < count; i++)
    drm_fence_put(fences[i]);
  kfree(fences);
}

static int drm_syncobj_wait_fences(struct drm_fence **fences, uint32_t count,
                                   bool wait_all, int64_t timeout_ns,
                                   uint32_t *first_signaled) {
  uint64_t deadline = timeout_ns < 0 ? UINT64_MAX : (uint64_t)timeout_ns;
  if (wait_all) {
    for (uint32_t i = 0; i < count; i++) {
      uint64_t now = sched_clock();
      uint64_t remaining = deadline == UINT64_MAX
                               ? UINT64_MAX
                               : (deadline > now ? deadline - now : 0);
      int rc = drm_fence_wait(fences[i], remaining);
      if (rc)
        return rc;
    }
    *first_signaled = 0;
    return 0;
  }

  for (;;) {
    for (uint32_t i = 0; i < count; i++) {
      if (drm_fence_is_signaled(fences[i])) {
        *first_signaled = i;
        return 0;
      }
    }
    uint64_t now = sched_clock();
    if (deadline != UINT64_MAX && now >= deadline)
      return -ETIMEDOUT;
    uint64_t slice = 1000000ULL;
    if (deadline != UINT64_MAX && deadline - now < slice)
      slice = deadline - now;
    int rc = drm_fence_wait(fences[0], slice);
    if (rc && rc != -ETIMEDOUT)
      return rc;
  }
}

static long drm_core_common_ioctl(xtask *proc, struct file *file,
                                  struct drm_core_file *df, uint32_t cmd,
                                  void *arg, bool *handled) {
  *handled = true;
  switch (cmd) {
  case DRM_IOCTL_SET_MASTER:
    if (df->render)
      return -EACCES;
    mutex_lock(&df->dev->file_mutex);
    if (df->dev->master && df->dev->master != df) {
      mutex_unlock(&df->dev->file_mutex);
      return -EBUSY;
    }
    df->dev->master = df;
    df->master = true;
    mutex_unlock(&df->dev->file_mutex);
    return 0;
  case DRM_IOCTL_DROP_MASTER:
    if (df->render)
      return -EACCES;
    mutex_lock(&df->dev->file_mutex);
    if (df->dev->master != df) {
      mutex_unlock(&df->dev->file_mutex);
      return -EPERM;
    }
    df->dev->master = NULL;
    df->master = false;
    mutex_unlock(&df->dev->file_mutex);
    spin_lock(&df->event_lock);
    df->event_armed = false;
    df->event_pending = false;
    spin_unlock(&df->event_lock);
    if (df->dev->master_drop)
      df->dev->master_drop(df->dev->driver_private);
    return 0;
  case DRM_IOCTL_GET_MAGIC: {
    if (df->render)
      return -EACCES;
    struct drm_auth *auth = arg;
    if (!auth)
      return -EFAULT;
    mutex_lock(&df->dev->file_mutex);
    auth->magic = ++df->dev->next_magic;
    if (!auth->magic)
      auth->magic = ++df->dev->next_magic;
    df->magic = auth->magic;
    df->authenticated = false;
    mutex_unlock(&df->dev->file_mutex);
    return 0;
  }
  case DRM_IOCTL_AUTH_MAGIC: {
    if (df->render)
      return -EACCES;
    struct drm_auth *auth = arg;
    if (!auth)
      return -EFAULT;
    mutex_lock(&df->dev->file_mutex);
    if (df->dev->master != df) {
      mutex_unlock(&df->dev->file_mutex);
      return -EPERM;
    }
    struct drm_core_file *target = NULL;
    for (list_node *node = df->dev->files.next; node != &df->dev->files;
         node = node->next) {
      struct drm_core_file *candidate =
          LIST_ENTRY(node, struct drm_core_file, node);
      if (candidate->magic == auth->magic) {
        target = candidate;
        break;
      }
    }
    if (target)
      target->authenticated = true;
    mutex_unlock(&df->dev->file_mutex);
    return target ? 0 : -EPERM;
  }
  case DRM_IOCTL_SET_CLIENT_CAP: {
    struct drm_set_client_cap *cap = arg;
    if (!cap)
      return -EFAULT;
    if (cap->value > 1 || cap->capability == 0 || cap->capability > 63)
      return -EINVAL;
    if (cap->capability == DRM_CLIENT_CAP_ATOMIC)
      return -EINVAL;
    if (cap->capability != DRM_CLIENT_CAP_UNIVERSAL_PLANES &&
        cap->capability != DRM_CLIENT_CAP_STEREO_3D &&
        cap->capability != DRM_CLIENT_CAP_ASPECT_RATIO)
      return -EINVAL;
    uint64_t bit = 1ULL << cap->capability;
    if (cap->value)
      df->client_caps |= bit;
    else
      df->client_caps &= ~bit;
    return 0;
  }
  case DRM_IOCTL_GEM_CLOSE: {
    struct drm_gem_close *close = arg;
    if (!close)
      return -EFAULT;
    return drm_core_gem_handle_delete(file, close->handle);
  }
  case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
    struct drm_prime_handle *prime = arg;
    if (!prime || !proc)
      return -EFAULT;
    if (prime->flags & ~(DRM_CLOEXEC | DRM_RDWR))
      return -EINVAL;
    struct drm_gem_object *object =
        drm_core_gem_object_lookup(file, prime->handle);
    if (!object)
      return -ENOENT;
    if (object->dmabuf) {
      dma_buf_get(object->dmabuf);
      int fd = dma_buf_fd_install(proc, object->dmabuf,
                                  (prime->flags & DRM_CLOEXEC) != 0);
      if (fd < 0) {
        dma_buf_put(object->dmabuf);
        drm_gem_object_put(object);
        return fd;
      }
      drm_gem_object_put(object);
      prime->fd = fd;
      return 0;
    }
    struct drm_prime_object *wrapper = kmalloc(sizeof(*wrapper));
    if (!wrapper) {
      drm_gem_object_put(object);
      return -ENOMEM;
    }
    wrapper->object = object;
    wrapper->handle_hint = prime->handle;
    wrapper->writable = (prime->flags & DRM_RDWR) != 0;
    int fd = bsd_drm_prime_fd_install(proc, wrapper,
                                      (prime->flags & DRM_CLOEXEC) != 0);
    if (fd < 0) {
      drm_prime_object_put(wrapper);
      return fd;
    }
    prime->fd = fd;
    return 0;
  }
  case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
    struct drm_prime_handle *prime = arg;
    if (!prime || !proc)
      return -EFAULT;
    struct dma_buf *dmabuf = dma_buf_get_from_fd(proc, prime->fd);
    if (dmabuf) {
      struct drm_gem_object *object = NULL;
      mutex_lock(&df->dev->file_mutex);
      for (size_t i = 0; i < DRM_CORE_MAX_HANDLES; i++) {
        struct drm_gem_object *candidate = df->handles[i].object;
        if (candidate && candidate->dmabuf == dmabuf) {
          object = candidate;
          drm_gem_object_get(object);
          break;
        }
      }
      mutex_unlock(&df->dev->file_mutex);
      if (!object) {
        uint32_t page_count = 0;
        struct page **source = dma_buf_pages(dmabuf, &page_count);
        struct page **pages =
            source ? kmalloc((size_t)page_count * sizeof(*pages)) : NULL;
        if (!pages) {
          dma_buf_put(dmabuf);
          return -ENOMEM;
        }
        __memcpy(pages, source, (size_t)page_count * sizeof(*pages));
        object = drm_gem_object_create(df->dev, dma_buf_size(dmabuf), pages,
                                       page_count, NULL, NULL);
        if (!object) {
          kfree(pages);
          dma_buf_put(dmabuf);
          return -ENOMEM;
        }
        object->dmabuf = dmabuf;
      } else {
        dma_buf_put(dmabuf);
      }
      uint32_t handle = 0;
      int rc = drm_core_gem_handle_create(file, object, 0, &handle);
      drm_gem_object_put(object);
      if (rc)
        return rc;
      prime->handle = handle;
      return 0;
    }
    struct file *prime_file = bsd_drm_prime_fd_get(proc, prime->fd);
    if (!prime_file)
      return -EBADF;
    struct drm_prime_object *wrapper = prime_file->drm_prime;
    uint32_t handle = 0;
    int rc = drm_core_gem_handle_create(file, wrapper->object,
                                        wrapper->handle_hint, &handle);
    file_put(prime_file);
    if (rc)
      return rc == -EINVAL ? -EXDEV : rc;
    prime->handle = handle;
    return 0;
  }
  case DRM_IOCTL_GET_CAP: {
    struct drm_get_cap *cap = arg;
    if (!cap)
      return -EFAULT;
    if (cap->capability == DRM_CAP_SYNCOBJ) {
      cap->value = 1;
      return 0;
    }
    if (cap->capability == DRM_CAP_SYNCOBJ_TIMELINE) {
      cap->value = 0;
      return 0;
    }
    *handled = false;
    return 0;
  }
  case DRM_IOCTL_SYNCOBJ_CREATE: {
    struct drm_syncobj_create *request = arg;
    if (!request)
      return -EFAULT;
    if (request->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
      return -EINVAL;
    return drm_syncobj_create(
        df, (request->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0,
        &request->handle);
  }
  case DRM_IOCTL_SYNCOBJ_DESTROY: {
    struct drm_syncobj_destroy *request = arg;
    if (!request)
      return -EFAULT;
    if (request->pad)
      return -EINVAL;
    mutex_lock(&df->dev->file_mutex);
    struct drm_core_syncobj *entry =
        drm_syncobj_find_locked(df, request->handle);
    struct drm_fence *fence = entry ? entry->fence : NULL;
    if (entry)
      __memset(entry, 0, sizeof(*entry));
    mutex_unlock(&df->dev->file_mutex);
    if (!entry)
      return -ENOENT;
    drm_fence_put(fence);
    return 0;
  }
  case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: {
    struct drm_syncobj_handle *request = arg;
    if (!request || !proc)
      return -EFAULT;
    if (request->flags != DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE ||
        request->pad || request->point)
      return -EINVAL;
    mutex_lock(&df->dev->file_mutex);
    struct drm_core_syncobj *entry =
        drm_syncobj_find_locked(df, request->handle);
    struct drm_fence *fence = entry ? entry->fence : NULL;
    drm_fence_get(fence);
    mutex_unlock(&df->dev->file_mutex);
    if (!fence)
      return -ENOENT;
    int fd = drm_fence_install_sync_file(fence, proc);
    drm_fence_put(fence);
    if (fd < 0)
      return fd;
    request->fd = fd;
    return 0;
  }
  case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: {
    struct drm_syncobj_handle *request = arg;
    if (!request || !proc)
      return -EFAULT;
    if (request->flags != DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE ||
        request->pad || request->point)
      return -EINVAL;
    struct file *sync_file = bsd_sync_file_fd_get(proc, request->fd);
    if (!sync_file)
      return -EBADF;
    struct drm_fence *imported = sync_file->sync_file_fence;
    drm_fence_get(imported);
    file_put(sync_file);
    uint32_t handle = 0;
    int rc = drm_syncobj_create(df, false, &handle);
    if (rc) {
      drm_fence_put(imported);
      return rc;
    }
    mutex_lock(&df->dev->file_mutex);
    struct drm_core_syncobj *entry = drm_syncobj_find_locked(df, handle);
    struct drm_fence *placeholder = entry->fence;
    entry->fence = imported;
    mutex_unlock(&df->dev->file_mutex);
    drm_fence_put(placeholder);
    request->handle = handle;
    return 0;
  }
  case DRM_IOCTL_SYNCOBJ_SIGNAL:
  case DRM_IOCTL_SYNCOBJ_RESET: {
    struct drm_syncobj_array *request = arg;
    if (!request)
      return -EFAULT;
    if (request->pad)
      return -EINVAL;
    struct drm_fence **fences = NULL;
    int rc = drm_syncobj_collect(df, request->handles, request->count_handles,
                                 &fences);
    if (rc)
      return rc;
    if (cmd == DRM_IOCTL_SYNCOBJ_SIGNAL) {
      for (uint32_t i = 0; i < request->count_handles; i++)
        drm_fence_signal(fences[i]);
      drm_syncobj_put_fences(fences, request->count_handles);
      return 0;
    }

    struct drm_fence **replacements =
        kmalloc(request->count_handles * sizeof(*replacements));
    if (!replacements) {
      drm_syncobj_put_fences(fences, request->count_handles);
      return -ENOMEM;
    }
    __memset(replacements, 0, request->count_handles * sizeof(*replacements));
    for (uint32_t i = 0; i < request->count_handles; i++) {
      replacements[i] = drm_fence_create(false);
      if (!replacements[i]) {
        for (uint32_t j = 0; j < i; j++)
          drm_fence_put(replacements[j]);
        kfree(replacements);
        drm_syncobj_put_fences(fences, request->count_handles);
        return -ENOMEM;
      }
    }
    uint32_t *handles = kmalloc(request->count_handles * sizeof(*handles));
    if (!handles || copy_from_user(handles, (void *)(uintptr_t)request->handles,
                                   request->count_handles * sizeof(*handles))) {
      for (uint32_t i = 0; i < request->count_handles; i++)
        drm_fence_put(replacements[i]);
      kfree(handles);
      kfree(replacements);
      drm_syncobj_put_fences(fences, request->count_handles);
      return handles ? -EFAULT : -ENOMEM;
    }
    mutex_lock(&df->dev->file_mutex);
    for (uint32_t i = 0; i < request->count_handles; i++) {
      if (!drm_syncobj_find_locked(df, handles[i])) {
        rc = -ENOENT;
        break;
      }
    }
    if (rc) {
      mutex_unlock(&df->dev->file_mutex);
      for (uint32_t i = 0; i < request->count_handles; i++)
        drm_fence_put(replacements[i]);
      kfree(handles);
      kfree(replacements);
      drm_syncobj_put_fences(fences, request->count_handles);
      return rc;
    }
    for (uint32_t i = 0; i < request->count_handles; i++) {
      struct drm_core_syncobj *entry = drm_syncobj_find_locked(df, handles[i]);
      struct drm_fence *old = entry->fence;
      entry->fence = replacements[i];
      replacements[i] = old;
    }
    mutex_unlock(&df->dev->file_mutex);
    for (uint32_t i = 0; i < request->count_handles; i++)
      drm_fence_put(replacements[i]);
    kfree(handles);
    kfree(replacements);
    drm_syncobj_put_fences(fences, request->count_handles);
    return 0;
  }
  case DRM_IOCTL_SYNCOBJ_WAIT: {
    struct drm_syncobj_wait *request = arg;
    if (!request)
      return -EFAULT;
    uint32_t allowed = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
                       DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                       DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
                       DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
    if ((request->flags & ~allowed) || request->pad)
      return -EINVAL;
    struct drm_fence **fences = NULL;
    int rc = drm_syncobj_collect(df, request->handles, request->count_handles,
                                 &fences);
    if (rc)
      return rc;
    rc = drm_syncobj_wait_fences(
        fences, request->count_handles,
        (request->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0,
        request->timeout_nsec, &request->first_signaled);
    drm_syncobj_put_fences(fences, request->count_handles);
    return rc == -ETIMEDOUT ? -ETIME : rc;
  }
  default:
    *handled = false;
    return 0;
  }
}

static bool drm_ioctl_requires_master(uint32_t cmd) {
  switch (cmd) {
  case DRM_IOCTL_MODE_SETCRTC:
  case DRM_IOCTL_MODE_PAGE_FLIP:
  case DRM_IOCTL_MODE_CURSOR:
  case DRM_IOCTL_MODE_CURSOR2:
  case DRM_IOCTL_MODE_DIRTYFB:
  case DRM_IOCTL_MODE_SETPROPERTY:
  case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
    return true;
  default:
    return false;
  }
}

static bool drm_ioctl_is_kms(uint32_t cmd) {
  switch (cmd) {
  case DRM_IOCTL_MODE_GETRESOURCES:
  case DRM_IOCTL_MODE_GETCRTC:
  case DRM_IOCTL_MODE_SETCRTC:
  case DRM_IOCTL_MODE_GETCONNECTOR:
  case DRM_IOCTL_MODE_GETENCODER:
  case DRM_IOCTL_MODE_GETPLANERESOURCES:
  case DRM_IOCTL_MODE_GETPLANE:
  case DRM_IOCTL_MODE_CREATE_DUMB:
  case DRM_IOCTL_MODE_MAP_DUMB:
  case DRM_IOCTL_MODE_DESTROY_DUMB:
  case DRM_IOCTL_MODE_ADDFB:
  case DRM_IOCTL_MODE_ADDFB2:
  case DRM_IOCTL_MODE_GETFB:
  case DRM_IOCTL_MODE_RMFB:
  case DRM_IOCTL_MODE_PAGE_FLIP:
  case DRM_IOCTL_MODE_CURSOR:
  case DRM_IOCTL_MODE_CURSOR2:
  case DRM_IOCTL_MODE_DIRTYFB:
  case DRM_IOCTL_MODE_GETPROPERTY:
  case DRM_IOCTL_MODE_GETPROPBLOB:
  case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
  case DRM_IOCTL_MODE_SETPROPERTY:
  case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
    return true;
  default:
    return false;
  }
}

static long drm_minor_ioctl_file(xtask *proc, struct file *file, uint32_t cmd,
                                 void *arg) {
  struct drm_core_minor *minor = drm_minor_from_file(file);
  if (!minor || !minor->dev ||
      drm_core_device_state(minor->dev) != DRM_CORE_REGISTERED)
    return -ENODEV;
  struct drm_core_file *df = drm_file_find(file);
  if (!df)
    return -EBADF;
  bool handled = false;
  long rc = drm_core_common_ioctl(proc, file, df, cmd, arg, &handled);
  if (handled)
    return rc;
  if (df->render && drm_ioctl_is_kms(cmd))
    return -EACCES;
  if (drm_ioctl_requires_master(cmd) && !df->master)
    return -EACCES;
  rc = drm_core_kms_ioctl(df, cmd, arg, &handled);
  if (handled)
    return rc;
  const struct dev_ops *backend = minor->type == DRM_NODE_PRIMARY
                                      ? minor->dev->primary_template
                                      : minor->dev->render_template;
  return backend && backend->ioctl_file
             ? backend->ioctl_file(proc, file, cmd, arg)
             : -ENOTTY;
}

static wait_queue_head *drm_minor_wait_queue_file(struct file *file) {
  struct drm_core_file *df = drm_file_find(file);
  return df ? &df->event_wq : NULL;
}

static __poll drm_minor_poll_file(xtask *proc, struct file *file, int events) {
  (void)proc;
  (void)events;
  struct drm_core_file *df = drm_file_find(file);
  if (!df || df->render)
    return 0;
  spin_lock(&df->event_lock);
  __poll result = df->event_pending ? POLLIN : 0;
  spin_unlock(&df->event_lock);
  return result;
}

static ssize_t drm_minor_read_file(xtask *proc, struct file *file, void *buf,
                                   size_t count) {
  (void)proc;
  struct drm_core_file *df = drm_file_find(file);
  if (!df || df->render)
    return -EACCES;
  if (count < sizeof(struct drm_event_vblank))
    return -EINVAL;
  spin_lock(&df->event_lock);
  if (!df->event_pending) {
    spin_unlock(&df->event_lock);
    return 0;
  }
  struct drm_event_vblank event;
  __memset(&event, 0, sizeof(event));
  event.base.type = DRM_EVENT_FLIP_COMPLETE;
  event.base.length = sizeof(event);
  event.user_data = df->event_user_data;
  event.sequence = df->event_sequence;
  event.crtc_id = df->event_crtc_id;
  uint64_t now = sched_clock();
  event.tv_sec = now / 1000000000ULL;
  event.tv_usec = (now % 1000000000ULL) / 1000ULL;
  df->event_pending = false;
  spin_unlock(&df->event_lock);
  return copy_to_user(buf, &event, sizeof(event)) ? -EFAULT
                                                  : (ssize_t)sizeof(event);
}

static void drm_gem_vma_get(void *owner) { drm_gem_object_get(owner); }

static void drm_gem_vma_put(void *owner) { drm_gem_object_put(owner); }

static const struct vma_owner_ops drm_gem_vma_owner_ops = {
    .get = drm_gem_vma_get,
    .put = drm_gem_vma_put,
};

static int drm_minor_mmap_prepare_file(struct file *file,
                                       const struct dev_mmap_request *request,
                                       struct dev_mmap_backing *backing) {
  if (!file || !request || !backing || !request->length ||
      (request->offset & (PAGE_SIZE - 1)))
    return -EINVAL;
  struct drm_core_file *df = drm_file_find(file);
  if (!df)
    return -EBADF;

  mutex_lock(&df->dev->file_mutex);
  struct drm_gem_object *object = NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_HANDLES; i++) {
    struct drm_gem_object *candidate = df->handles[i].object;
    if (candidate && candidate->mmap_offset == request->offset) {
      object = candidate;
      drm_gem_object_get(object);
      break;
    }
  }
  mutex_unlock(&df->dev->file_mutex);
  if (!object)
    return -EACCES;
  if (request->length > ALIGN_UP(object->size, PAGE_SIZE) ||
      request->length / PAGE_SIZE > object->page_count) {
    drm_gem_object_put(object);
    return -EINVAL;
  }

  backing->owner = object;
  backing->owner_ops = &drm_gem_vma_owner_ops;
  backing->pages = object->pages;
  backing->page_count = request->length / PAGE_SIZE;
  backing->cache_flags = 0;
  return 0;
}

int drm_prime_mmap_prepare(struct drm_prime_object *prime,
                           const struct dev_mmap_request *request,
                           struct dev_mmap_backing *backing) {
  if (!prime || !prime->object || !request || !backing ||
      !request->requested_length || request->offset != 0 ||
      !(request->flags & MAP_SHARED) || (request->flags & MAP_PRIVATE) ||
      (request->flags & MAP_ANONYMOUS) || (request->prot & PROT_EXEC))
    return -EINVAL;
  if ((request->prot & PROT_WRITE) && !prime->writable)
    return -EACCES;

  struct drm_gem_object *object = prime->object;
  if (!object->size || !object->pages || !object->page_count ||
      request->requested_length > object->size ||
      request->length / PAGE_SIZE > object->page_count)
    return -EINVAL;

  drm_gem_object_get(object);
  backing->owner = object;
  backing->owner_ops = &drm_gem_vma_owner_ops;
  backing->pages = object->pages;
  backing->page_count = request->length / PAGE_SIZE;
  backing->cache_flags = 0;
  return 0;
}

struct drm_core_device *
drm_core_device_alloc(const struct drm_core_config *config) {
  if (!config || !config->driver_name || !config->driver_name[0] ||
      (!config->primary_ops && !config->render_ops))
    return NULL;
  struct drm_core_device *dev = kmalloc(sizeof(*dev));
  if (!dev)
    return NULL;
  __memset(dev, 0, sizeof(*dev));
  refcount_set(&dev->refcount, 1);
  dev->state = DRM_CORE_INITIALIZED;
  dev->slot = -1;
  __strncpy(dev->driver_name, config->driver_name,
            sizeof(dev->driver_name) - 1);
  __strncpy(dev->subsystem_target,
            config->subsystem_target ? config->subsystem_target
                                     : "/sys/class/drm",
            sizeof(dev->subsystem_target) - 1);
  dev->primary_template = config->primary_ops;
  dev->render_template = config->render_ops;
  dev->driver_private = config->driver_private;
  dev->master_drop = config->master_drop;
  dev->driver_release = config->driver_release;
  mutex_init(&dev->file_mutex);
  list_init(&dev->files);
  dev->next_object_id = 1;
  dev->next_property_id = 1;
  dev->next_blob_id = 1;
  dev->next_mmap_page = DRM_CORE_MMAP_OFFSET_START;
  dev->primary.dev = dev;
  dev->primary.type = DRM_NODE_PRIMARY;
  dev->render.dev = dev;
  dev->render.type = DRM_NODE_RENDER;
  return dev;
}

static int drm_minor_publish(struct drm_core_device *dev,
                             struct drm_core_minor *minor,
                             const struct dev_ops *ops_template) {
  if (!ops_template)
    return -EINVAL;
  unsigned index = (unsigned)dev->slot;
  minor->minor_id = minor->type == DRM_NODE_PRIMARY ? index : 128 + index;
  if (minor->type == DRM_NODE_PRIMARY) {
    snprintf(minor->sysfs_name, sizeof(minor->sysfs_name), "card%u", index);
  } else {
    snprintf(minor->sysfs_name, sizeof(minor->sysfs_name), "renderD%u",
             128 + index);
  }
  snprintf(minor->dev_name, sizeof(minor->dev_name), "dri/%s",
           minor->sysfs_name);

  minor->ops = *ops_template;
  refcount_set(&minor->ops.refcount, 0);
  minor->ops.storage = DEV_OPS_OWNER_EMBEDDED;
  minor->ops.release = drm_minor_ops_release;
  minor->ops.instance_priv = minor;
  minor->ops.sysfs_dir = NULL;
  minor->ops.uevent_priv = NULL;
  minor->ops.minor = minor->minor_id;
  minor->ops.open = NULL;
  minor->ops.open_file = drm_minor_open_file;
  minor->ops.close_file = drm_minor_close_file;
  minor->ops.ioctl_file = drm_minor_ioctl_file;
  minor->ops.mmap = NULL;
  minor->ops.mmap_file = NULL;
  minor->ops.mmap_prepare_file = drm_minor_mmap_prepare_file;
  if (minor->type == DRM_NODE_PRIMARY) {
    minor->ops.read = NULL;
    minor->ops.poll = NULL;
    minor->ops.read_file = drm_minor_read_file;
    minor->ops.poll_file = drm_minor_poll_file;
    minor->ops.wait_queue_file = drm_minor_wait_queue_file;
  }
  __memset(minor->ops.subsystem, 0, sizeof(minor->ops.subsystem));
  __memset(minor->ops.devtype, 0, sizeof(minor->ops.devtype));
  __strncpy(minor->ops.subsystem, "drm", sizeof(minor->ops.subsystem) - 1);
  __strncpy(minor->ops.devtype,
            minor->type == DRM_NODE_PRIMARY ? "card" : "render",
            sizeof(minor->ops.devtype) - 1);

  drm_core_device_get(dev);
  int rc = devtmpfs_create(minor->dev_name, &minor->ops, NULL);
  if (rc) {
    drm_core_device_put(dev);
    return rc;
  }
  minor->published = true;

#ifdef TEST
  if (minor->type == DRM_NODE_RENDER &&
      drm_core_fault(DRM_CORE_FAULT_RENDER_PUBLISH))
    return -ENOMEM;
#endif

  minor->class_node = sysfs_create_dir(drm_class_dir, minor->sysfs_name);
  if (!minor->class_node)
    return -ENOMEM;
  minor->ops.sysfs_dir = minor->class_node;
  minor->driver_attr = (struct sysfs_attr){
      .name = "driver", .priv = minor, .show = drm_minor_driver_show};
  minor->dev_attr = (struct sysfs_attr){
      .name = "dev", .priv = minor, .show = drm_minor_dev_show};
  struct sysfs_node *driver_file =
      sysfs_create_file(minor->class_node, "driver", &minor->driver_attr);
  struct sysfs_node *dev_file =
      sysfs_create_file(minor->class_node, "dev", &minor->dev_attr);
  if (!driver_file || !dev_file)
    return -ENOMEM;
  if (sysfs_node_set_owner(driver_file, dev, &drm_owner_ops) ||
      sysfs_node_set_owner(dev_file, dev, &drm_owner_ops))
    return -ENOMEM;

  minor->devchar_node = sysfs_devchar_register(
      DRM_CORE_MAJOR, minor->minor_id, minor->dev_name, dev->subsystem_target);
  if (!minor->devchar_node)
    return -ENOMEM;
  return 0;
}

static void drm_minor_unpublish(struct drm_core_minor *minor) {
  if (!minor || !minor->published)
    return;
  devtmpfs_remove(minor->dev_name);
  sysfs_devchar_unregister(DRM_CORE_MAJOR, minor->minor_id);
  if (minor->class_node)
    sysfs_remove_dir(minor->class_node);
  minor->ops.sysfs_dir = NULL;
  minor->class_node = NULL;
  minor->devchar_node = NULL;
  minor->published = false;
}

int drm_core_device_register(struct drm_core_device *dev, uint32_t node_mask) {
  if (!dev || !node_mask || (node_mask & ~(DRM_NODE_PRIMARY | DRM_NODE_RENDER)))
    return -EINVAL;
  if ((node_mask & DRM_NODE_PRIMARY) && !dev->primary_template)
    return -EINVAL;
  if ((node_mask & DRM_NODE_RENDER) && !dev->render_template)
    return -EINVAL;

  mutex_lock(&drm_registry_mutex);
  if (dev->state != DRM_CORE_INITIALIZED) {
    mutex_unlock(&drm_registry_mutex);
    return -EBUSY;
  }
  int slot = -1;
  for (int i = 0; i < DRM_CORE_SLOTS; i++) {
    if (!drm_slots[i]) {
      drm_slots[i] = true;
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    mutex_unlock(&drm_registry_mutex);
    return -ENOSPC;
  }
  dev->slot = slot;
  __atomic_store_n(&dev->state, DRM_CORE_REGISTERING, __ATOMIC_RELEASE);
  mutex_unlock(&drm_registry_mutex);

  int rc = 0;
  if (node_mask & DRM_NODE_PRIMARY)
    rc = drm_minor_publish(dev, &dev->primary, dev->primary_template);
  if (!rc && (node_mask & DRM_NODE_RENDER))
    rc = drm_minor_publish(dev, &dev->render, dev->render_template);
  if (!rc && dev->primary.devchar_node && dev->render.devchar_node) {
    sysfs_devchar_add_device_child(dev->primary.devchar_node, "drm",
                                   dev->primary.sysfs_name);
    sysfs_devchar_add_device_child(dev->primary.devchar_node, "drm",
                                   dev->render.sysfs_name);
    sysfs_devchar_add_device_child(dev->render.devchar_node, "drm",
                                   dev->primary.sysfs_name);
    sysfs_devchar_add_device_child(dev->render.devchar_node, "drm",
                                   dev->render.sysfs_name);
  }
  if (rc) {
    drm_minor_unpublish(&dev->render);
    drm_minor_unpublish(&dev->primary);
    mutex_lock(&drm_registry_mutex);
    drm_slots[dev->slot] = false;
    dev->slot = -1;
    __atomic_store_n(&dev->state, DRM_CORE_INITIALIZED, __ATOMIC_RELEASE);
    mutex_unlock(&drm_registry_mutex);
    return rc;
  }

  mutex_lock(&drm_registry_mutex);
  dev->node_mask = node_mask;
  __atomic_store_n(&dev->state, DRM_CORE_REGISTERED, __ATOMIC_RELEASE);
  mutex_unlock(&drm_registry_mutex);
  printk(LOG_INFO, "drm_core: %s registered slot=%d nodes=0x%x\n",
         dev->driver_name, dev->slot, node_mask);
  return 0;
}

void drm_core_device_unregister(struct drm_core_device *dev) {
  if (!dev)
    return;
  mutex_lock(&drm_registry_mutex);
  if (dev->state != DRM_CORE_REGISTERED) {
    mutex_unlock(&drm_registry_mutex);
    return;
  }
  __atomic_store_n(&dev->state, DRM_CORE_UNPLUGGED, __ATOMIC_RELEASE);
  int slot = dev->slot;
  mutex_unlock(&drm_registry_mutex);

  drm_minor_unpublish(&dev->render);
  drm_minor_unpublish(&dev->primary);

  mutex_lock(&drm_registry_mutex);
  if (slot >= 0)
    drm_slots[slot] = false;
  dev->slot = -1;
  dev->node_mask = 0;
  mutex_unlock(&drm_registry_mutex);
}

int drm_core_device_slot(const struct drm_core_device *dev) {
  return dev ? dev->slot : -1;
}

enum drm_core_state drm_core_device_state(const struct drm_core_device *dev) {
  return dev ? __atomic_load_n(&dev->state, __ATOMIC_ACQUIRE)
             : DRM_CORE_UNPLUGGED;
}

void *drm_core_driver_private(const struct drm_core_device *dev) {
  return dev ? dev->driver_private : NULL;
}

struct drm_core_device *drm_core_file_device(struct file *file) {
  struct drm_core_minor *minor = drm_minor_from_file(file);
  return minor ? minor->dev : NULL;
}

bool drm_core_file_is_master(struct file *file) {
  struct drm_core_file *df = drm_file_find(file);
  return df && df->master;
}

bool drm_core_file_is_authenticated(struct file *file) {
  struct drm_core_file *df = drm_file_find(file);
  return df && (df->render || df->master || df->authenticated);
}

struct drm_gem_object *
drm_gem_object_create(struct drm_core_device *dev, uint64_t size,
                      struct page **pages, uint32_t page_count, void *private,
                      const struct drm_gem_object_ops *ops) {
  if (!dev || !size || !pages || !page_count ||
      page_count != ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE)
    return NULL;
  struct drm_gem_object *object = kmalloc(sizeof(*object));
  if (!object)
    return NULL;
  __memset(object, 0, sizeof(*object));
  refcount_set(&object->refcount, 1);
  object->reservation_lock = (spinlock)SPINLOCK_INIT;
  object->dev = dev;
  object->size = size;
  object->pages = pages;
  object->page_count = page_count;
  object->private = private;
  object->ops = ops;
  drm_core_device_get(dev);

  mutex_lock(&dev->file_mutex);
  if (dev->next_mmap_page > UINT64_MAX / PAGE_SIZE - page_count) {
    mutex_unlock(&dev->file_mutex);
    drm_core_device_put(dev);
    kfree(object);
    return NULL;
  }
  object->mmap_offset = dev->next_mmap_page * PAGE_SIZE;
  dev->next_mmap_page += page_count;
  mutex_unlock(&dev->file_mutex);
  return object;
}

void drm_gem_object_get(struct drm_gem_object *object) {
  if (object)
    refcount_inc(&object->refcount);
}

void drm_gem_object_put(struct drm_gem_object *object) {
  if (!object || !refcount_dec_and_test(&object->refcount))
    return;
  drm_fence_put(object->exclusive_fence);
  if (object->ops && object->ops->release)
    object->ops->release(object);
  dma_buf_put(object->dmabuf);
  kfree(object->pages);
  drm_core_device_put(object->dev);
  kfree(object);
}

void drm_prime_object_put(struct drm_prime_object *object) {
  if (!object)
    return;
  drm_gem_object_put(object->object);
  kfree(object);
}

uint64_t drm_prime_object_size(const struct drm_prime_object *object) {
  return object && object->object ? object->object->size : 0;
}

uint64_t drm_prime_object_id(const struct drm_prime_object *object) {
  if (!object || !object->object)
    return 0;
  return object->object->mmap_offset / PAGE_SIZE;
}

bool drm_prime_object_cpu_access_ready(const struct drm_prime_object *object) {
  if (!object || !object->object)
    return false;
  struct drm_fence *fence = drm_gem_reservation_get_exclusive(object->object);
  if (!fence)
    return true;
  bool ready = drm_fence_is_signaled(fence);
  drm_fence_put(fence);
  return ready;
}

void *drm_gem_object_private(struct drm_gem_object *object) {
  return object ? object->private : NULL;
}

uint64_t drm_gem_object_size(const struct drm_gem_object *object) {
  return object ? object->size : 0;
}

struct page **drm_gem_object_pages(struct drm_gem_object *object,
                                   uint32_t *page_count) {
  if (!object || !page_count)
    return NULL;
  *page_count = object->page_count;
  return object->pages;
}

void drm_gem_reservation_set_exclusive(struct drm_gem_object *object,
                                       struct drm_fence *fence) {
  if (!object)
    return;
  if (object->dmabuf) {
    if (fence)
      (void)dma_resv_add_fence(&object->dmabuf->resv, fence, true);
    return;
  }
  if (fence)
    drm_fence_get(fence);
  uint64_t flags;
  spin_lock_irqsave(&object->reservation_lock, &flags);
  struct drm_fence *old = object->exclusive_fence;
  object->exclusive_fence = fence;
  spin_unlock_irqrestore(&object->reservation_lock, flags);
  drm_fence_put(old);
}

struct drm_fence *
drm_gem_reservation_get_exclusive(struct drm_gem_object *object) {
  if (!object)
    return NULL;
  if (object->dmabuf)
    return dma_resv_export_fence(&object->dmabuf->resv, false);
  uint64_t flags;
  spin_lock_irqsave(&object->reservation_lock, &flags);
  struct drm_fence *fence = object->exclusive_fence;
  drm_fence_get(fence);
  spin_unlock_irqrestore(&object->reservation_lock, flags);
  return fence;
}

int drm_core_gem_handle_create(struct file *file, struct drm_gem_object *object,
                               uint32_t preferred_handle, uint32_t *handle) {
  struct drm_core_file *df = drm_file_find(file);
  if (!df || !object || !handle || object->dev != df->dev)
    return -EINVAL;
  mutex_lock(&df->dev->file_mutex);
  struct drm_core_handle *free_entry = NULL;
  bool preferred_busy = false;
  for (size_t i = 0; i < DRM_CORE_MAX_HANDLES; i++) {
    struct drm_core_handle *entry = &df->handles[i];
    if (entry->object == object) {
      *handle = entry->handle;
      mutex_unlock(&df->dev->file_mutex);
      return 0;
    }
    if (!entry->object && !free_entry)
      free_entry = entry;
    if (preferred_handle && entry->handle == preferred_handle && entry->object)
      preferred_busy = true;
  }
  if (!free_entry) {
    mutex_unlock(&df->dev->file_mutex);
    return -ENOSPC;
  }
  uint32_t candidate =
      preferred_handle && !preferred_busy ? preferred_handle : df->next_handle;
  if (!candidate)
    candidate = 1;
  for (;;) {
    bool busy = false;
    for (size_t i = 0; i < DRM_CORE_MAX_HANDLES; i++)
      busy |= df->handles[i].object && df->handles[i].handle == candidate;
    if (!busy)
      break;
    if (++candidate == 0) {
      mutex_unlock(&df->dev->file_mutex);
      return -ENOSPC;
    }
  }
  free_entry->handle = candidate;
  free_entry->object = object;
  drm_gem_object_get(object);
  df->next_handle = candidate + 1;
  *handle = candidate;
  mutex_unlock(&df->dev->file_mutex);
  return 0;
}

struct drm_gem_object *drm_core_gem_object_lookup(struct file *file,
                                                  uint32_t handle) {
  struct drm_core_file *df = drm_file_find(file);
  if (!df || !handle)
    return NULL;
  mutex_lock(&df->dev->file_mutex);
  struct drm_gem_object *object = NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_HANDLES; i++) {
    if (df->handles[i].object && df->handles[i].handle == handle) {
      object = df->handles[i].object;
      drm_gem_object_get(object);
      break;
    }
  }
  mutex_unlock(&df->dev->file_mutex);
  return object;
}

int drm_core_gem_handle_delete(struct file *file, uint32_t handle) {
  struct drm_core_file *df = drm_file_find(file);
  if (!df || !handle)
    return -ENOENT;
  mutex_lock(&df->dev->file_mutex);
  struct drm_gem_object *object = NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_HANDLES; i++) {
    if (df->handles[i].object && df->handles[i].handle == handle) {
      object = df->handles[i].object;
      df->handles[i].handle = 0;
      df->handles[i].object = NULL;
      break;
    }
  }
  mutex_unlock(&df->dev->file_mutex);
  if (!object)
    return -ENOENT;
  drm_gem_object_put(object);
  return 0;
}

int drm_core_gem_mmap_offset(struct file *file, uint32_t handle,
                             uint64_t *offset) {
  if (!offset)
    return -EINVAL;
  struct drm_gem_object *object = drm_core_gem_object_lookup(file, handle);
  if (!object)
    return -ENOENT;
  *offset = object->mmap_offset;
  drm_gem_object_put(object);
  return 0;
}

uint32_t drm_core_object_alloc(struct drm_core_device *dev) {
  return drm_core_object_create(dev, 0);
}

uint32_t drm_core_object_create(struct drm_core_device *dev,
                                uint32_t object_type) {
  if (!dev)
    return 0;
  mutex_lock(&dev->file_mutex);
  struct drm_core_object *object = NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_OBJECTS; i++) {
    if (!dev->objects[i].id) {
      object = &dev->objects[i];
      break;
    }
  }
  uint32_t id = 0;
  if (object) {
    id = dev->next_object_id++;
    if (!id)
      id = dev->next_object_id++;
    object->id = id;
    object->type = object_type;
  }
  mutex_unlock(&dev->file_mutex);
  return id;
}

static struct drm_core_property *
drm_property_alloc(struct drm_core_device *dev, const char *name,
                   enum drm_core_property_type type, bool immutable) {
  if (!dev || !name || !name[0])
    return NULL;
  struct drm_core_property *property = NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_PROPERTIES; i++) {
    if (!dev->properties[i].id) {
      property = &dev->properties[i];
      break;
    }
  }
  if (!property)
    return NULL;
  property->id = dev->next_property_id++;
  if (!property->id)
    property->id = dev->next_property_id++;
  property->type = type;
  property->immutable = immutable;
  __strncpy(property->name, name, sizeof(property->name) - 1);
  return property;
}

uint32_t drm_core_property_create_range(struct drm_core_device *dev,
                                        const char *name, uint64_t min,
                                        uint64_t max, bool immutable) {
  if (!dev || min > max)
    return 0;
  mutex_lock(&dev->file_mutex);
  struct drm_core_property *property =
      drm_property_alloc(dev, name, DRM_CORE_PROPERTY_RANGE, immutable);
  if (property) {
    property->min = min;
    property->max = max;
  }
  uint32_t id = property ? property->id : 0;
  mutex_unlock(&dev->file_mutex);
  return id;
}

uint32_t drm_core_property_create_enum(struct drm_core_device *dev,
                                       const char *name, const uint64_t *values,
                                       const char *const *names, size_t count,
                                       bool immutable) {
  if (!dev || !values || !names || !count ||
      count > DRM_CORE_MAX_PROPERTY_ENUMS)
    return 0;
  mutex_lock(&dev->file_mutex);
  struct drm_core_property *property =
      drm_property_alloc(dev, name, DRM_CORE_PROPERTY_ENUM, immutable);
  if (property) {
    property->enum_count = count;
    for (size_t i = 0; i < count; i++) {
      property->enums[i].value = values[i];
      __strncpy(property->enums[i].name, names[i],
                sizeof(property->enums[i].name) - 1);
    }
  }
  uint32_t id = property ? property->id : 0;
  mutex_unlock(&dev->file_mutex);
  return id;
}

uint32_t drm_core_property_create_blob(struct drm_core_device *dev,
                                       const char *name, bool immutable) {
  if (!dev)
    return 0;
  mutex_lock(&dev->file_mutex);
  struct drm_core_property *property =
      drm_property_alloc(dev, name, DRM_CORE_PROPERTY_BLOB, immutable);
  uint32_t id = property ? property->id : 0;
  mutex_unlock(&dev->file_mutex);
  return id;
}

uint32_t drm_core_property_create_object(struct drm_core_device *dev,
                                         const char *name, uint32_t object_type,
                                         bool immutable) {
  if (!dev || !object_type)
    return 0;
  mutex_lock(&dev->file_mutex);
  struct drm_core_property *property =
      drm_property_alloc(dev, name, DRM_CORE_PROPERTY_OBJECT, immutable);
  if (property)
    property->object_type = object_type;
  uint32_t id = property ? property->id : 0;
  mutex_unlock(&dev->file_mutex);
  return id;
}

uint32_t drm_core_blob_create(struct drm_core_device *dev, const void *data,
                              size_t length) {
  if (!dev || !data || !length || length > UINT32_MAX)
    return 0;
  void *copy = kmalloc(length);
  if (!copy)
    return 0;
  __memcpy(copy, data, length);
  mutex_lock(&dev->file_mutex);
  struct drm_core_blob *blob = NULL;
  for (size_t i = 0; i < DRM_CORE_MAX_BLOBS; i++) {
    if (!dev->blobs[i].id) {
      blob = &dev->blobs[i];
      break;
    }
  }
  uint32_t id = 0;
  if (blob) {
    id = dev->next_blob_id++;
    if (!id)
      id = dev->next_blob_id++;
    blob->id = id;
    blob->length = length;
    blob->data = copy;
  }
  mutex_unlock(&dev->file_mutex);
  if (!blob)
    kfree(copy);
  return id;
}

int drm_core_object_add_property(struct drm_core_device *dev,
                                 uint32_t object_id, uint32_t object_type,
                                 uint32_t property_id, uint64_t value) {
  if (!dev)
    return -EINVAL;
  mutex_lock(&dev->file_mutex);
  struct drm_core_object *object = drm_object_find(dev, object_id, object_type);
  struct drm_core_property *property = drm_property_find(dev, property_id);
  if (!object || !property) {
    mutex_unlock(&dev->file_mutex);
    return -ENOENT;
  }
  if (object->property_count >= DRM_CORE_MAX_OBJECT_PROPERTIES) {
    mutex_unlock(&dev->file_mutex);
    return -ENOSPC;
  }
  size_t slot = object->property_count++;
  object->property_ids[slot] = property_id;
  object->property_values[slot] = value;
  mutex_unlock(&dev->file_mutex);
  return 0;
}

int drm_core_object_property_by_name(struct drm_core_device *dev,
                                     uint32_t object_id, uint32_t object_type,
                                     const char *name, uint32_t *property_id,
                                     uint64_t *value) {
  if (!dev || !name)
    return -EINVAL;
  mutex_lock(&dev->file_mutex);
  struct drm_core_object *object = drm_object_find(dev, object_id, object_type);
  if (!object) {
    mutex_unlock(&dev->file_mutex);
    return -ENOENT;
  }
  for (size_t i = 0; i < object->property_count; i++) {
    struct drm_core_property *property =
        drm_property_find(dev, object->property_ids[i]);
    if (property && !__strncmp(property->name, name, sizeof(property->name))) {
      if (property_id)
        *property_id = property->id;
      if (value)
        *value = object->property_values[i];
      mutex_unlock(&dev->file_mutex);
      return 0;
    }
  }
  mutex_unlock(&dev->file_mutex);
  return -ENOENT;
}

int drm_core_event_queue(struct file *file, uint64_t user_data,
                         uint32_t crtc_id, uint64_t deadline_ns) {
  struct drm_core_file *df = drm_file_find(file);
  if (!df || df->render)
    return -EACCES;
  spin_lock(&df->event_lock);
  if (df->event_armed || df->event_pending) {
    spin_unlock(&df->event_lock);
    return -EBUSY;
  }
  df->event_armed = true;
  df->event_deadline_ns = deadline_ns;
  df->event_user_data = user_data;
  df->event_crtc_id = crtc_id;
  spin_unlock(&df->event_lock);
  return 0;
}

void drm_core_event_tick(struct drm_core_device *dev, uint64_t now_ns) {
  if (!dev)
    return;
  mutex_lock(&dev->file_mutex);
  for (list_node *node = dev->files.next; node != &dev->files;
       node = node->next) {
    struct drm_core_file *df = LIST_ENTRY(node, struct drm_core_file, node);
    bool wake = false;
    spin_lock(&df->event_lock);
    if (df->event_armed && now_ns >= df->event_deadline_ns) {
      df->event_armed = false;
      df->event_pending = true;
      df->event_sequence++;
      wake = true;
    }
    spin_unlock(&df->event_lock);
    if (wake)
      __wake_up(&df->event_wq, POLLIN);
  }
  mutex_unlock(&dev->file_mutex);
}

struct sysfs_node *drm_core_class_node(struct drm_core_device *dev,
                                       uint32_t node_type) {
  struct drm_core_minor *minor = drm_minor_for_type(dev, node_type);
  return minor ? minor->class_node : NULL;
}

struct sysfs_node *drm_core_devchar_node(struct drm_core_device *dev,
                                         uint32_t node_type) {
  struct drm_core_minor *minor = drm_minor_for_type(dev, node_type);
  return minor ? minor->devchar_node : NULL;
}
