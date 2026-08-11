/* Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_COMPOSITOR_VULKAN_PRIME_PROBE_H
#define OS_COMPOSITOR_VULKAN_PRIME_PROBE_H

#include <stdbool.h>

struct wlr_allocator;
struct wlr_renderer;

/* Exercises a real LINEAR dma-buf/PRIME import before the Wayland socket
 * exists. */
bool os_vulkan_prime_probe(struct wlr_renderer *renderer,
                           struct wlr_allocator *allocator, int drm_fd);

#endif
