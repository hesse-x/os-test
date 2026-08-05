/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OS_CURSOR_CURSORS_H
#define OS_CURSOR_CURSORS_H

struct os_cursor_def {
  const char *name;         /* wlroots/xcursor cursor name */
  const char *path;         /* image-absolute path for runtime fopen */
  int hotspot_x, hotspot_y; /* 128px asset coords */
};

static const struct os_cursor_def os_cursors[] = {
    /* pointers */
    {"left_ptr", "/usr/share/cursors/left_ptr.png", 4, 4},
    {"right_ptr", "/usr/share/cursors/right_ptr.png", 124, 4},
    {"center_ptr", "/usr/share/cursors/center_ptr.png", 64, 4},
    {"arrow", "/usr/share/cursors/left_ptr.png", 4, 4},
    {"default", "/usr/share/cursors/left_ptr.png", 4, 4},
    /* text I-beam */
    {"xterm", "/usr/share/cursors/xterm.png", 64, 64},
    {"text", "/usr/share/cursors/xterm.png", 64, 64},
    {"vertical-text", "/usr/share/cursors/vertical-text.png", 64, 64},
    /* hand / link / dnd */
    {"hand2", "/usr/share/cursors/hand2.png", 32, 20},
    {"hand1", "/usr/share/cursors/hand1.png", 32, 20},
    {"pointer", "/usr/share/cursors/hand2.png", 32, 20},
    {"link", "/usr/share/cursors/link.png", 52, 20},
    {"copy", "/usr/share/cursors/copy.png", 52, 20},
    {"dnd_no_drop", "/usr/share/cursors/dnd_no_drop.png", 52, 20},
    {"dnd-move", "/usr/share/cursors/move.png", 64, 64},
    {"crossed_circle", "/usr/share/cursors/crossed_circle.png", 52, 20},
    {"context-menu", "/usr/share/cursors/context-menu.png", 4, 4},
    {"wayland-cursor", "/usr/share/cursors/wayland-cursor.png", 64, 64},
    /* move / crosshair / dots */
    {"move", "/usr/share/cursors/move.png", 64, 64},
    {"crosshair", "/usr/share/cursors/crosshair.png", 64, 64},
    {"cross", "/usr/share/cursors/cross.png", 64, 64},
    {"dotbox", "/usr/share/cursors/dotbox.png", 64, 64},
    {"plus", "/usr/share/cursors/plus.png", 64, 64},
    /* resize: double arrows (edges) */
    {"sb_h_double_arrow", "/usr/share/cursors/sb_h_double_arrow.png", 64, 64},
    {"sb_v_double_arrow", "/usr/share/cursors/sb_v_double_arrow.png", 64, 64},
    /* resize: single arrows */
    {"sb_left_arrow", "/usr/share/cursors/sb_left_arrow.png", 64, 64},
    {"sb_right_arrow", "/usr/share/cursors/sb_right_arrow.png", 64, 64},
    {"sb_up_arrow", "/usr/share/cursors/sb_up_arrow.png", 64, 64},
    {"sb_down_arrow", "/usr/share/cursors/sb_down_arrow.png", 64, 64},
    /* resize: sides. left_side/top_side have dedicated assets; right_side/
     * bottom_side are absent from the set, so fall back to the double-arrow. */
    {"left_side", "/usr/share/cursors/left_side.png", 64, 64},
    {"right_side", "/usr/share/cursors/sb_h_double_arrow.png", 64, 64},
    {"top_side", "/usr/share/cursors/top_side.png", 64, 64},
    {"bottom_side", "/usr/share/cursors/sb_v_double_arrow.png", 64, 64},
    /* resize: corners — hotspot at the actual corner. top corners reuse the
     * ul/ur angle assets; bottom corners have dedicated images. */
    {"top_left_corner", "/usr/share/cursors/ul_angle.png", 8, 8},
    {"top_right_corner", "/usr/share/cursors/ur_angle.png", 120, 8},
    {"bottom_left_corner", "/usr/share/cursors/bottom_left_corner.png", 8, 120},
    {"bottom_right_corner", "/usr/share/cursors/bottom_right_corner.png", 120,
     120},
    {"ul_angle", "/usr/share/cursors/ul_angle.png", 8, 8},
    {"ur_angle", "/usr/share/cursors/ur_angle.png", 120, 8},
    {"ll_angle", "/usr/share/cursors/ll_angle.png", 8, 120},
    {"lr_angle", "/usr/share/cursors/lr_angle.png", 120, 120},
    {"all-scroll", "/usr/share/cursors/all-scroll.png", 64, 64},
    /* tees */
    {"left_tee", "/usr/share/cursors/left_tee.png", 64, 64},
    {"right_tee", "/usr/share/cursors/right_tee.png", 64, 64},
    {"top_tee", "/usr/share/cursors/top_tee.png", 64, 64},
    {"bottom_tee", "/usr/share/cursors/bottom_tee.png", 64, 64},
    /* misc */
    {"X_cursor", "/usr/share/cursors/X_cursor.png", 64, 64},
    {"pencil", "/usr/share/cursors/pencil.png", 8, 124},
    {"person", "/usr/share/cursors/person.png", 32, 16},
    {"pin", "/usr/share/cursors/pin.png", 32, 20},
    {"question_arrow", "/usr/share/cursors/question_arrow.png", 8, 8},
    {"zoom-in", "/usr/share/cursors/zoom-in.png", 64, 64},
    {"zoom-out", "/usr/share/cursors/zoom-out.png", 64, 64},
    /* Animation sequences wait/ and left_ptr_watch/ are packaged but not played
     * this phase; a name match falls back to the default cursor. */
    {"wait", "/usr/share/cursors/left_ptr.png", 4, 4},
    {"left_ptr_watch", "/usr/share/cursors/left_ptr.png", 4, 4},
};
static const int os_cursors_n = sizeof(os_cursors) / sizeof(os_cursors[0]);

#endif
