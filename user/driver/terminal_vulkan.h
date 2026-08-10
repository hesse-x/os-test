/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_display;
struct wl_surface;

struct TerminalVulkanRenderer;

struct TerminalRect {
  float x, y, width, height;
  float color[4];
};

struct TerminalGlyphQuad {
  float x, y, width, height;
  float u0, v0, u1, v1;
  float color[4];
};

template <typename T> struct TerminalBatch {
  const T *items;
  size_t count;
};

struct TerminalDrawList {
  TerminalBatch<TerminalRect> backgrounds;
  TerminalBatch<TerminalGlyphQuad> glyphs;
  TerminalBatch<TerminalRect> decorations;
  TerminalBatch<TerminalRect> cursor;
  TerminalBatch<TerminalGlyphQuad> cursor_glyphs;
};

struct TerminalFrame {
  const TerminalDrawList *draw_list;
  const uint8_t *atlas_pixels;
  uint32_t atlas_width;
  uint32_t atlas_height;
  uint64_t atlas_generation;
  uint32_t logical_width;
  uint32_t logical_height;
};

enum TerminalVkRenderResult {
  TERMINAL_VK_OK,
  TERMINAL_VK_RETRY,
  TERMINAL_VK_FATAL,
};

typedef void (*TerminalVkFrameReady)(void *data);

bool terminal_vk_create(TerminalVulkanRenderer **out, wl_display *display,
                        wl_surface *surface, uint32_t logical_width,
                        uint32_t logical_height, uint32_t buffer_scale,
                        TerminalVkFrameReady frame_ready, void *frame_data);
void terminal_vk_resize(TerminalVulkanRenderer *renderer,
                        uint32_t logical_width, uint32_t logical_height,
                        uint32_t buffer_scale);
TerminalVkRenderResult terminal_vk_render(TerminalVulkanRenderer *renderer,
                                          const TerminalFrame *frame);
void terminal_vk_destroy(TerminalVulkanRenderer **renderer);
