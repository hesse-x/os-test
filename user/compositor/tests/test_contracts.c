/*
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "../core/lifetime.h"
#include "../dock/dock_layout.h"
#include "../frame/frame_plan.h"
#include "../frame/scene_adapter.h"
#include "../protocols/effect_manager.h"
#include "../protocols/effect_state.h"
#include "../protocols/trusted_client.h"
#include "../renderer/effects/blur.h"
#include "../renderer/effects/visual.h"
#include "../renderer/mesh/mesh.h"
#include "../sdk/osgui.h"
#include "../window/animation/genie_mesh.h"
#include "../window/window_state.h"

static bool identity_deform(void *data, float u, float v, float progress,
                            struct os_vec2 *out) {
  (void)data;
  (void)progress;
  out->x = u;
  out->y = v;
  return true;
}

static bool legacy_commit(void *output) { return output == (void *)1; }
static bool scene_snapshot(void *data, struct wlr_scene_output *output,
                           const struct os_scene_draw_item **items,
                           size_t *item_count) {
  (void)data;
  (void)output;
  (void)items;
  (void)item_count;
  return true;
}

static void test_window_state(void) {
  struct os_window_state_machine state;
  os_window_state_init(&state);
  assert(state.state == OS_WINDOW_VISIBLE);
  assert(os_window_request_minimize(&state) == OS_WINDOW_TRANSITION_OK);
  assert(os_window_reverse_animation(&state) == OS_WINDOW_TRANSITION_OK);
  assert(os_window_finish_animation(&state) == OS_WINDOW_TRANSITION_OK);
  assert(state.state == OS_WINDOW_VISIBLE);
  assert(os_window_request_restore(&state) == OS_WINDOW_TRANSITION_NOOP);
  assert(os_window_begin_destroy(&state) == OS_WINDOW_TRANSITION_OK);
  assert(os_window_request_minimize(&state) == OS_WINDOW_TRANSITION_INVALID);
  assert(os_window_begin_destroy(NULL) == OS_WINDOW_TRANSITION_INVALID);
}

static void test_lifetime(void) {
  struct os_lifetime_counters counters = {0};
  assert(os_lifetime_acquire(&counters, OS_LIVE_WINDOW));
  assert(os_lifetime_total(&counters) == 1);
  assert(os_lifetime_release(&counters, OS_LIVE_WINDOW));
  assert(!os_lifetime_release(&counters, OS_LIVE_WINDOW));
  assert(os_lifetime_total(&counters) == 0);
}

static void test_dock_layout(void) {
  struct os_dock_layout layout;
  assert(!os_dock_layout_init(&layout, 1280, 720, 0));
  assert(os_dock_layout_init(&layout, 1280, 720, 2));
  const uint64_t ids[] = {11, 12, 13};
  assert(os_dock_layout_set_targets(&layout, ids, 3));
  assert(layout.width ==
         (layout.icon_size + 2 * layout.padding) * (int)layout.target_count);
  assert(layout.height ==
         ((layout.icon_size + 2 * layout.padding) * 10 + 8) / 9);
  struct os_dock_target target;
  assert(os_dock_layout_target(&layout, 12, &target));
  assert(target.dock_target_id == 12 && target.geometry.scale == 2);
  assert(target.generation == layout.generation);
  uint64_t hit = 0;
  assert(os_dock_layout_hit_test(&layout, layout.width / 2.0,
                                 layout.height / 2.0, &hit));
  assert(hit == 12);
  struct os_dock_icon_geometry idle[3], hovered[3];
  os_dock_layout_icons(&layout, 0, false, idle);
  os_dock_layout_icons(&layout, layout.width / 2.0, true, hovered);
  assert(hovered[1].size > idle[1].size);
  assert(fabs(hovered[1].size - idle[1].size * OS_DOCK_HOVER_SCALE) < 0.001);
  assert(hovered[0].x < idle[0].x);
  assert(hovered[2].x > idle[2].x);
  assert(hovered[0].size > idle[0].size && hovered[0].size < hovered[1].size);
  const uint64_t duplicate[] = {4, 4};
  assert(!os_dock_layout_set_targets(&layout, duplicate, 2));
}

static void test_effect_commit(void) {
  struct os_effect_state state;
  os_effect_state_init(&state);
  const struct os_effect_value value = {
      .enabled = true, .radius = 16, .x = 1, .y = 2, .width = 40, .height = 20};
  assert(os_effect_set_pending(&state, &value, 32));
  assert(!state.current.enabled);
  assert(os_effect_apply_surface_commit(&state));
  assert(state.current.enabled && state.current.radius == 16);
  assert(!os_effect_apply_surface_commit(&state));
  struct os_effect_value bad = value;
  bad.width = -1;
  assert(!os_effect_set_pending(&state, &bad, 32));
}

static void test_effect_manager_and_blur(void) {
  const struct wl_client *client = (const struct wl_client *)(uintptr_t)1;
  const struct wl_client *other = (const struct wl_client *)(uintptr_t)2;
  const struct wl_surface *surface = (const struct wl_surface *)(uintptr_t)3;
  struct os_effect_manager manager;
  os_effect_manager_init(&manager, client, 24, 10000);
  assert(os_effect_manager_can_bind(&manager, client));
  assert(!os_effect_manager_can_bind(&manager, other));
  struct os_effect_surface *effect =
      os_effect_manager_create_surface(&manager, client, surface);
  assert(effect != NULL && os_effect_manager_surface_count(&manager) == 1);
  assert(os_effect_manager_create_surface(&manager, client, surface) == NULL);
  struct os_effect_value value = {.enabled = true,
                                  .radius = 12,
                                  .x = 10,
                                  .y = 10,
                                  .width = 80,
                                  .height = 60};
  assert(os_effect_surface_set_blur(&manager, effect, &value));
  assert(!effect->state.current.enabled);
  assert(os_effect_surface_commit(effect) && effect->state.current.enabled);
  value.width = 200;
  value.height = 200;
  assert(!os_effect_surface_set_blur(&manager, effect, &value));
  os_effect_manager_destroy_surface(&manager, surface);
  assert(os_effect_manager_surface_count(&manager) == 0);

  struct os_effect_graph graph;
  os_effect_graph_init(&graph, NULL);
  struct os_blur_node node = {.surface_id = 1,
                              .effect_generation = 2,
                              .region = {100, 100, 320, 80},
                              .radius = 16,
                              .downsample = 4,
                              .tint_rgba8 = 0xffffff20};
  assert(os_effect_graph_add_blur(&graph, &node, 1280, 720));
  assert(!os_effect_graph_add_blur(&graph, &node, 1280, 720));
  struct os_effect_rect expanded;
  assert(
      os_blur_damage_expand(&node.region, node.radius, 1280, 720, &expanded));
  assert(expanded.x == 84 && expanded.width == 352);
  struct os_blur_cache cache = {.captured_region = expanded, .valid = true};
  struct os_effect_rect damage = {90, 90, 2, 2};
  os_blur_cache_invalidate_damage(&cache, &damage);
  assert(!cache.valid);

  float weights[OS_BLUR_MAX_RADIUS + 1];
  size_t count;
  assert(os_blur_kernel(3, weights, 33, &count) && count == 4);
  float sum = weights[0];
  for (size_t i = 1; i < count; ++i)
    sum += 2.0f * weights[i];
  assert(fabsf(sum - 1.0f) < 0.0001f);
}

static void test_visual_primitives(void) {
  assert(fabsf(os_srgb_encode(os_srgb_decode(0.5f)) - 0.5f) < 0.0001f);
  struct os_linear_color red = {1.0f, 0.0f, 0.0f, 0.5f};
  uint32_t encoded = os_color_encode_rgba8(red, OS_PIXEL_ALPHA_PREMULTIPLIED);
  struct os_linear_color decoded =
      os_color_decode_rgba8(encoded, OS_PIXEL_ALPHA_PREMULTIPLIED);
  assert(decoded.a > 0.49f && decoded.a < 0.51f && decoded.r > 0.99f);
  struct os_visual_rect rect = {.x = 10,
                                .y = 10,
                                .width = 100,
                                .height = 60,
                                .radius = 12,
                                .border_width = 2};
  assert(os_rounded_rect_coverage(&rect, 60, 40, 1) == 1.0f);
  assert(os_rounded_rect_coverage(&rect, 9, 9, 1) == 0.0f);
  assert(os_rounded_rect_border_coverage(&rect, 60, 10, 1) > 0.0f);
  rect.rotation_radians = 3.14159265f / 2.0f;
  float x = 0, y = 0;
  os_visual_rotate_point(&rect, 110, 40, &x, &y);
  assert(fabsf(x - 60) < 0.001f && fabsf(y - 90) < 0.001f);

  struct os_shadow_cache cache;
  os_shadow_cache_init(&cache);
  struct os_shadow_key key = {
      .width = 100, .height = 60, .radius_q8 = 12 * 256};
  os_shadow_cache_insert(&cache, &key, 42, NULL);
  uint64_t id = 0;
  assert(os_shadow_cache_lookup(&cache, &key, &id) && id == 42);
}

static void test_frame_and_mesh(void) {
  struct os_frame_plan plan = {.size = sizeof(plan),
                               .version = OS_FRAME_PLAN_VERSION,
                               .full_damage = true};
  assert(os_frame_plan_validate(&plan));
  plan.version = 99;
  assert(!os_frame_plan_validate(&plan));
  assert(os_frame_submit_legacy((void *)1, legacy_commit));
  assert(!os_frame_submit_legacy(NULL, legacy_commit));
  struct os_scene_adapter adapter = {.size = sizeof(adapter),
                                     .version = OS_SCENE_ADAPTER_VERSION,
                                     .snapshot = scene_snapshot};
  assert(os_scene_adapter_validate(&adapter));

  struct os_mesh_vertex vertices[9];
  uint32_t indices[24];
  struct os_mesh mesh = {.vertices = vertices,
                         .indices = indices,
                         .vertex_capacity = 9,
                         .index_capacity = 24};
  const struct os_grid_spec spec = {
      .size = sizeof(spec), .columns = 2, .rows = 2, .color_rgba8 = UINT32_MAX};
  assert(os_mesh_build_grid(&spec, identity_deform, NULL, 0.5f, &mesh));
  assert(mesh.vertex_count == 9 && mesh.index_count == 24);
  assert(!os_mesh_build_grid(&spec, identity_deform, NULL, NAN, &mesh));
}

static void test_trusted_client(void) {
  struct os_trusted_client trusted = {0};
  const struct wl_client *client = (const struct wl_client *)(uintptr_t)1;
  const struct wl_client *other = (const struct wl_client *)(uintptr_t)2;
  assert(os_trusted_client_init(&trusted, client));
  assert(!os_trusted_client_init(&trusted, other));
  assert(os_trusted_client_matches(&trusted, client));
  assert(!os_trusted_client_matches(&trusted, other));
}

static void test_osgui_abi(void) {
  osgui_context *context = (osgui_context *)(uintptr_t)1;
  struct osgui_context_options options = {.size = sizeof(options),
                                          .version = OSGUI_API_VERSION};
  assert(osgui_context_create(&options, &context) == OSGUI_OK);
  osgui_frame *frame = NULL, *second = (osgui_frame *)(uintptr_t)1;
  assert(osgui_begin_frame(context, 800, 600, &frame) == OSGUI_OK);
  struct osgui_rect rect = {10, 10, 100, 40};
  struct osgui_rect_style style = {
      .size = sizeof(style), .radius = 8, .border_width = 1};
  assert(osgui_draw_rounded_rect(frame, &rect, &style) == OSGUI_OK);
  assert(osgui_frame_command_count(frame) == 1);
  assert(osgui_frame_command(frame, 0)->type == OSGUI_COMMAND_ROUNDED_RECT);
  assert(osgui_begin_frame(context, 1, 1, &second) == OSGUI_INVALID_STATE);
  assert(second == NULL);
  assert(osgui_end_frame(NULL) == OSGUI_INVALID_ARGUMENT);
  assert(osgui_end_frame(frame) == OSGUI_OK);
  osgui_context_destroy(context);

  context = (osgui_context *)(uintptr_t)1;
  options.size = 0;
  assert(osgui_context_create(&options, &context) == OSGUI_INVALID_ARGUMENT);
  assert(context == NULL);
}

static int snapshot_releases;
static void release_snapshot(void *handle) {
  assert(handle == (void *)(uintptr_t)7);
  ++snapshot_releases;
}

static void test_genie_mesh(void) {
  struct os_genie_animation animation;
  struct os_genie_snapshot snapshot = {.handle = (void *)(uintptr_t)7,
                                       .width = 640,
                                       .height = 480,
                                       .release = release_snapshot};
  struct os_genie_target target = {
      .id = 4, .generation = 8, .x = 600, .y = 680, .width = 48, .height = 8};
  assert(os_genie_begin(&animation, &snapshot, &target, 80, 60, 640, 480,
                        1000000000, OS_GENIE_MINIMIZE));
  struct os_mesh_vertex vertices[25];
  uint32_t indices[96];
  struct os_mesh mesh = {.vertices = vertices,
                         .indices = indices,
                         .vertex_capacity = 25,
                         .index_capacity = 96};
  assert(os_genie_step(&animation, 0, &target, &mesh, 4, 4));
  assert(fabsf(vertices[0].position[0] - 80.0f) < 0.001f);
  assert(fabsf(vertices[0].position[1] - 60.0f) < 0.001f);
  assert(fabsf(vertices[24].position[0] - 720.0f) < 0.001f);
  assert(fabsf(vertices[24].position[1] - 540.0f) < 0.001f);
  assert(os_genie_step(&animation, 250000000, &target, &mesh, 4, 4));
  assert(mesh.vertex_count == 25 && animation.progress > 0.24f);
  assert(fabsf(vertices[0].position[0] - 80.0f) < 0.001f);
  assert(fabsf(vertices[4].position[0] - 720.0f) < 0.001f);
  assert(fabsf(vertices[0].position[1] - 60.0f) < 0.001f);
  assert(vertices[20].position[1] > 540.0f);
  assert(os_genie_reverse(&animation));
  assert(os_genie_step(&animation, 250000000, &target, &mesh, 4, 4));
  assert(animation.status == OS_GENIE_FINISHED && snapshot_releases == 1);

  target.generation++;
  snapshot.handle = (void *)(uintptr_t)7;
  assert(os_genie_begin(&animation, &snapshot, &target, 80, 60, 640, 480,
                        1000000000, OS_GENIE_MINIMIZE));
  struct os_genie_target stale = target;
  stale.generation--;
  assert(!os_genie_step(&animation, 1, &stale, &mesh, 4, 4));
  assert(snapshot_releases == 2);

  snapshot.handle = (void *)(uintptr_t)7;
  assert(os_genie_begin(&animation, &snapshot, &target, 80, 60, 640, 480,
                        1000000000, OS_GENIE_MINIMIZE));
  for (int frame = 0; frame < 20; ++frame)
    assert(os_genie_step(&animation, 50000000, &target, &mesh, 4, 4));
  assert(animation.status == OS_GENIE_FINISHED && snapshot_releases == 3);
  assert(fabsf(vertices[0].position[0] - target.x) < 0.001f);
  assert(fabsf(vertices[0].position[1] - target.y) < 0.001f);
  assert(fabsf(vertices[24].position[0] - (target.x + target.width)) < 0.001f);
  assert(fabsf(vertices[24].position[1] - (target.y + target.height)) < 0.001f);
}

int main(void) {
  test_window_state();
  test_lifetime();
  test_dock_layout();
  test_effect_commit();
  test_effect_manager_and_blur();
  test_visual_primitives();
  test_frame_and_mesh();
  test_trusted_client();
  test_osgui_abi();
  test_genie_mesh();
  puts("compositor contracts: PASS");
  return 0;
}
