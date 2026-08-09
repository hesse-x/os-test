/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_DRIVER_DRM_CORE_H
#define KERNEL_DRIVER_DRM_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct file;
struct page;

#define DRM_NODE_PRIMARY (1u << 0)
#define DRM_NODE_RENDER (1u << 1)

#define DRM_CORE_PROP_NAME_LEN 32

enum drm_core_state {
  DRM_CORE_INITIALIZED = 0,
  DRM_CORE_REGISTERING,
  DRM_CORE_REGISTERED,
  DRM_CORE_UNPLUGGED,
};

struct drm_core_device;
struct drm_gem_object;

struct drm_prime_object {
  struct drm_gem_object *object;
  uint32_t handle_hint;
};

struct drm_gem_object_ops {
  void (*release)(struct drm_gem_object *object);
};

struct drm_core_config {
  const char *driver_name;
  const char *subsystem_target;
  const struct dev_ops *primary_ops;
  const struct dev_ops *render_ops;
  void *driver_private;
  void (*master_drop)(void *driver_private);
};

void drm_core_init(void);
struct drm_core_device *
drm_core_device_alloc(const struct drm_core_config *config);
void drm_core_device_get(struct drm_core_device *dev);
void drm_core_device_put(struct drm_core_device *dev);
int drm_core_device_register(struct drm_core_device *dev, uint32_t node_mask);
void drm_core_device_unregister(struct drm_core_device *dev);

int drm_core_device_slot(const struct drm_core_device *dev);
enum drm_core_state drm_core_device_state(const struct drm_core_device *dev);
void *drm_core_driver_private(const struct drm_core_device *dev);
struct drm_core_device *drm_core_file_device(struct file *file);
bool drm_core_file_is_master(struct file *file);
bool drm_core_file_is_authenticated(struct file *file);
struct drm_gem_object *
drm_gem_object_create(struct drm_core_device *dev, uint64_t size,
                      struct page **pages, uint32_t page_count, void *private,
                      const struct drm_gem_object_ops *ops);
void drm_gem_object_get(struct drm_gem_object *object);
void drm_gem_object_put(struct drm_gem_object *object);
void drm_prime_object_put(struct drm_prime_object *object);
void *drm_gem_object_private(struct drm_gem_object *object);
uint64_t drm_gem_object_size(const struct drm_gem_object *object);
int drm_core_gem_handle_create(struct file *file, struct drm_gem_object *object,
                               uint32_t preferred_handle, uint32_t *handle);
struct drm_gem_object *drm_core_gem_object_lookup(struct file *file,
                                                  uint32_t handle);
int drm_core_gem_handle_delete(struct file *file, uint32_t handle);
int drm_core_gem_mmap_offset(struct file *file, uint32_t handle,
                             uint64_t *offset);
uint32_t drm_core_object_alloc(struct drm_core_device *dev);
uint32_t drm_core_object_create(struct drm_core_device *dev,
                                uint32_t object_type);
uint32_t drm_core_property_create_range(struct drm_core_device *dev,
                                        const char *name, uint64_t min,
                                        uint64_t max, bool immutable);
uint32_t drm_core_property_create_enum(struct drm_core_device *dev,
                                       const char *name, const uint64_t *values,
                                       const char *const *names, size_t count,
                                       bool immutable);
uint32_t drm_core_property_create_blob(struct drm_core_device *dev,
                                       const char *name, bool immutable);
uint32_t drm_core_property_create_object(struct drm_core_device *dev,
                                         const char *name, uint32_t object_type,
                                         bool immutable);
uint32_t drm_core_blob_create(struct drm_core_device *dev, const void *data,
                              size_t length);
int drm_core_object_add_property(struct drm_core_device *dev,
                                 uint32_t object_id, uint32_t object_type,
                                 uint32_t property_id, uint64_t value);
int drm_core_object_property_by_name(struct drm_core_device *dev,
                                     uint32_t object_id, uint32_t object_type,
                                     const char *name, uint32_t *property_id,
                                     uint64_t *value);
int drm_core_event_queue(struct file *file, uint64_t user_data,
                         uint32_t crtc_id, uint64_t deadline_ns);
void drm_core_event_tick(struct drm_core_device *dev, uint64_t now_ns);
struct sysfs_node *drm_core_class_node(struct drm_core_device *dev,
                                       uint32_t node_type);
struct sysfs_node *drm_core_devchar_node(struct drm_core_device *dev,
                                         uint32_t node_type);

#ifdef TEST
enum drm_core_fault_point {
  DRM_CORE_FAULT_NONE = 0,
  DRM_CORE_FAULT_RENDER_PUBLISH,
};

void drm_core_test_fail_once(enum drm_core_fault_point point);
void drm_mock_register_test_device(void);
#else
static inline void drm_mock_register_test_device(void) {}
#endif

#endif
