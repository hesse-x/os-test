/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Apple-style software cursor rendering for the tinywl compositor.
 *
 * Loads the MIT-original PNGs in user/cursor/ into pre-multiplied ARGB8888
 * self-built wlr_buffers (one per cursor, cached for the compositor lifetime)
 * and feeds them to wlr_cursor_set_buffer, reusing wlroots' software-cursor
 * GLES2 texture-quad path. See cursor.md for the full design.
 */
#ifndef TINYWL_CURSOR_H
#define TINYWL_CURSOR_H

struct wlr_cursor;
struct wlr_xcursor_manager;

/*
 * Load all cursor PNGs and build their cached wlr_buffers. Call once after the
 * xcursor manager is created and before wl_display_run. Self-built buffers do
 * not depend on the allocator/renderer, so renderer readiness is not required.
 * A cursor whose PNG is missing/corrupt or whose allocation fails is left with
 * a NULL buffer and falls back to the wlroots default at apply time; the
 * compositor never aborts.
 */
void os_cursor_init(void);

/*
 * Apply the cursor named <name> (a literal wlroots/xcursor name such as
 * "left_ptr", "move", "sb_h_double_arrow") to <cursor>. On a cache hit with a
 * non-NULL buffer this calls wlr_cursor_set_buffer with the cursor's hotspot;
 * otherwise it falls back to wlr_cursor_set_xcursor(..., "default") (NULL theme
 * → wlroots' built-in basic arrow). <mgr> is only used by the fallback path.
 */
void os_cursor_apply(struct wlr_cursor *cursor, struct wlr_xcursor_manager *mgr,
                     const char *name);

/*
 * Release all cached cursor buffers. Call after wl_display_run returns.
 */
void os_cursor_fini(void);

#endif
