/*
 * Comprehensive stubs for libinput/linker dependencies not available in our
 * minimal libc or evdev shim.  libinput's keyboard-subset pulls in pointer-
 * acceleration filter code and tablet/touchpad dispatch paths even for
 * keyboard-only devices; these stubs provide the missing symbols so the link
 * succeeds.  They are never called at runtime for keyboard operation.
 *
 * SPDX-License-Identifier: MIT
 *
 * NOTE: do NOT #include <math.h> — our math.h defines static inline wrappers
 * that conflict with the real function definitions needed here.  The math
 * stubs forward-declare their own signatures.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ===================== math stubs ===================== */

double sin(double x) {
  (void)x;
  return 0.0;
}
double cos(double x) {
  (void)x;
  return 1.0;
}
double atan2(double y, double x) {
  (void)y;
  (void)x;
  return 0.0;
}
double fmod(double x, double y) {
  (void)x;
  (void)y;
  return 0.0;
}
double hypot(double x, double y) {
  (void)x;
  (void)y;
  return 0.0;
}
double sqrt(double x) {
  (void)x;
  return 0.0;
}
void sincos(double x, double *s, double *c) {
  (void)x;
  *s = 0.0;
  *c = 1.0;
}

/* ===================== filter stubs ===================== */
/*
 * Keyboard-only terminal never uses pointer acceleration; returning NULL is
 * safe because all callers in evdev.c check for NULL.
 */

struct motion_filter;
struct motion_filter *create_pointer_accelerator_filter_flat(int dpi) {
  (void)dpi;
  return NULL;
}
struct motion_filter *
create_pointer_accelerator_filter_linear(int dpi, bool use_velocity_averaging) {
  (void)dpi;
  (void)use_velocity_averaging;
  return NULL;
}
struct motion_filter *
create_pointer_accelerator_filter_linear_low_dpi(int dpi,
                                                 bool use_velocity_averaging) {
  (void)dpi;
  (void)use_velocity_averaging;
  return NULL;
}
struct motion_filter *create_pointer_accelerator_filter_touchpad(
    int dpi, uint64_t event_delta_smooth_threshold,
    uint64_t event_delta_smooth_value, bool use_velocity_averaging) {
  (void)dpi;
  (void)event_delta_smooth_threshold;
  (void)event_delta_smooth_value;
  (void)use_velocity_averaging;
  return NULL;
}
struct motion_filter *create_pointer_accelerator_filter_touchpad_flat(int dpi) {
  (void)dpi;
  return NULL;
}
struct motion_filter *
create_pointer_accelerator_filter_lenovo_x230(int dpi,
                                              bool use_velocity_averaging) {
  (void)dpi;
  (void)use_velocity_averaging;
  return NULL;
}
struct motion_filter *
create_pointer_accelerator_filter_trackpoint(double multiplier,
                                             bool use_velocity_averaging) {
  (void)multiplier;
  (void)use_velocity_averaging;
  return NULL;
}
struct motion_filter *
create_pointer_accelerator_filter_trackpoint_flat(double multiplier) {
  (void)multiplier;
  return NULL;
}
struct motion_filter *create_pointer_accelerator_filter_tablet(int xres,
                                                               int yres) {
  (void)xres;
  (void)yres;
  return NULL;
}
struct motion_filter *create_custom_accelerator_filter(void) { return NULL; }

/* ===================== evdev dispatch stubs ===================== */
/*
 * Tablet/touchpad/totem dispatchers that libinput's evdev_configure_device
 * may reference but are never instantiated for a keyboard-only device.
 */

struct evdev_device;
struct evdev_dispatch *evdev_mt_touchpad_create(struct evdev_device *device) {
  (void)device;
  return NULL;
}
struct evdev_dispatch *evdev_tablet_create(struct evdev_device *device) {
  (void)device;
  return NULL;
}
struct evdev_dispatch *evdev_tablet_pad_create(struct evdev_device *device) {
  (void)device;
  return NULL;
}
struct evdev_dispatch *evdev_totem_create(struct evdev_device *device) {
  (void)device;
  return NULL;
}
int evdev_device_tablet_pad_get_num_buttons(struct evdev_device *device) {
  (void)device;
  return 0;
}
int evdev_device_tablet_pad_get_num_dials(struct evdev_device *device) {
  (void)device;
  return 0;
}
int evdev_device_tablet_pad_get_num_rings(struct evdev_device *device) {
  (void)device;
  return 0;
}
int evdev_device_tablet_pad_get_num_strips(struct evdev_device *device) {
  (void)device;
  return 0;
}
int evdev_device_tablet_pad_has_key(struct evdev_device *device) {
  (void)device;
  return 0;
}
int evdev_device_tablet_pad_get_num_mode_groups(struct evdev_device *device) {
  (void)device;
  return 0;
}
int evdev_device_tablet_pad_get_mode_group(struct evdev_device *device) {
  (void)device;
  return 0;
}

/* ===================== libevdev stubs ===================== */

struct libevdev;

int libevdev_set_abs_resolution(struct libevdev *evdev, unsigned int code,
                                int abs_min, int abs_max, int abs_fuzz,
                                int abs_flat) {
  (void)evdev;
  (void)code;
  (void)abs_min;
  (void)abs_max;
  (void)abs_fuzz;
  (void)abs_flat;
  return 0;
}
int libevdev_set_abs_fuzz(struct libevdev *evdev, unsigned int code, int fuzz) {
  (void)evdev;
  (void)code;
  (void)fuzz;
  return 0;
}
int libevdev_get_id_bustype(struct libevdev *evdev) {
  (void)evdev;
  return 0;
}
int libevdev_get_id_vendor(struct libevdev *evdev) {
  (void)evdev;
  return 0;
}
int libevdev_get_id_product(struct libevdev *evdev) {
  (void)evdev;
  return 0;
}
int libevdev_has_event_type(struct libevdev *evdev, unsigned int type) {
  (void)evdev;
  (void)type;
  return 0;
}
int libevdev_has_property(struct libevdev *evdev, unsigned int prop) {
  (void)evdev;
  (void)prop;
  return 0;
}
void libevdev_set_device_log_function(struct libevdev *evdev, void *log,
                                      void *data) {
  (void)evdev;
  (void)log;
  (void)data;
}
int libevdev_enable_event_type(struct libevdev *evdev, unsigned int type) {
  (void)evdev;
  (void)type;
  return 0;
}
int libevdev_disable_event_type(struct libevdev *evdev, unsigned int type) {
  (void)evdev;
  (void)type;
  return 0;
}
int libevdev_enable_property(struct libevdev *evdev, unsigned int prop) {
  (void)evdev;
  (void)prop;
  return 0;
}
int libevdev_disable_property(struct libevdev *evdev, unsigned int prop) {
  (void)evdev;
  (void)prop;
  return 0;
}
int libevdev_set_abs_maximum(struct libevdev *evdev, unsigned int code,
                             int val) {
  (void)evdev;
  (void)code;
  (void)val;
  return 0;
}
