/* Copyright (c) 2026 hesse
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
#include "../protocols/effect_state.h"
#include "../protocols/trusted_client.h"
#include "../renderer/mesh/mesh.h"
#include "../sdk/osgui.h"
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
  struct os_dock_target target;
  assert(os_dock_layout_target(&layout, 12, &target));
  assert(target.dock_target_id == 12 && target.geometry.scale == 2);
  assert(target.generation == layout.generation);
  uint64_t hit = 0;
  assert(os_dock_layout_hit_test(&layout, layout.width / 2.0,
                                 layout.height / 2.0, &hit));
  assert(hit == 12);
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

int main(void) {
  test_window_state();
  test_lifetime();
  test_dock_layout();
  test_effect_commit();
  test_frame_and_mesh();
  test_trusted_client();
  test_osgui_abi();
  puts("compositor contracts: PASS");
  return 0;
}
