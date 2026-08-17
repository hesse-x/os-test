/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_CORE_LIFETIME_H
#define OS_COMPOSITOR_CORE_LIFETIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum os_live_resource {
  OS_LIVE_OUTPUT,
  OS_LIVE_WINDOW,
  OS_LIVE_KEYBOARD,
  OS_LIVE_LAYER_SURFACE,
  OS_LIVE_POPUP,
  OS_LIVE_RESOURCE_COUNT,
};

struct os_lifetime_counters {
  size_t live[OS_LIVE_RESOURCE_COUNT];
};

bool os_lifetime_acquire(struct os_lifetime_counters *counters,
                         enum os_live_resource resource);
bool os_lifetime_release(struct os_lifetime_counters *counters,
                         enum os_live_resource resource);
size_t os_lifetime_total(const struct os_lifetime_counters *counters);

#endif
