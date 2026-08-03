/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Terminal process (libinput variant): VT100 state machine + cell buffer +
// compositor. Uses libinput for keyboard event processing via
// libinput_udev_create_context() + libinput_udev_assign_seat() — device
// add/remove is managed automatically by the udev backend monitor; the
// terminal receives hotplug uevents over the udevd pipe.
// Keyboard events reach libinput via the evdev broker: /dev/input/eventN
// consumer fds (in-kernel read/poll, per-client kfifo).
//
// fd 0 = stdout pipe read end (reads shell output, O_NONBLOCK)
// fd 1 = stdin pipe write end  (sends keystrokes to shell)
//
// Dynamic ELF (PT_INTERP → ld.so → libc.so + libinput.so).

#include "user/driver/display.h"
#include "utils/macro.h"
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
extern "C" {
#include <libseat.h>
}
#include <libudev.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <xos/ipc.h>

static int master_fd = -1;
static pid_t shell_pid = -1;

#define MAX_SEAT_INPUT_DEVICES 32

struct seat_input_device {
  int fd;
  int id;
};

struct terminal_seat {
  struct libseat *seat;
  struct libinput *libinput;
  struct seat_input_device inputs[MAX_SEAT_INPUT_DEVICES];
  int drm_fd;
  int drm_id;
  int active;
  int initialized;
  int fatal;
};

static struct terminal_seat terminal_seat;

// ===================== VT100 state =====================

#define VT100_NORMAL 0
#define VT100_ESC 1
#define VT100_CSI 2

struct cell {
  uint8_t ch;
  uint32_t fg_color;
  uint32_t bg_color;
};

struct vt100_state {
  int cursor_x;
  int cursor_y;
  int cols;
  int rows;
  uint32_t fg_color;
  uint32_t bg_color;
  int escape_state;
  int csi_params[8];
  int csi_param_count;
  int csi_private; // DEC private marker '?' seen right after CSI introducer
  int saved_x, saved_y; // DECSC \e7 / DECRC \e8 and SCOSC \e[s / SCORC \e[u
  int cursor_visible;   // DEC private mode ?25 (block cursor on/off)
};

static struct cell *cells;
static int dirty_row_start;
static int dirty_row_end;

static struct vt100_state vt;

// Block-cursor overlay: the inverted cursor is painted on top of the cell
// buffer at flush time, and restored to normal before the next repaint.
static int cursor_drawn; // an inverted cursor is currently on the back buffer
static int prev_cx, prev_cy; // cell the inverted cursor was last painted on

static const uint32_t vt100_colors[] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080,
    0x008080, 0xC0C0C0, 0x808080, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

static void mark_dirty(int row) {
  if (row < dirty_row_start)
    dirty_row_start = row;
  if (row + 1 > dirty_row_end)
    dirty_row_end = row + 1;
}

static void cell_putc(char c) {
  if (c == '\n') {
    vt.cursor_x = 0;
    if (vt.cursor_y < vt.rows - 1) {
      vt.cursor_y++;
    } else {
      for (int r = 0; r < vt.rows - 1; r++)
        for (int col = 0; col < vt.cols; col++)
          cells[r * vt.cols + col] = cells[(r + 1) * vt.cols + col];
      for (int col = 0; col < vt.cols; col++) {
        cells[(vt.rows - 1) * vt.cols + col].ch = ' ';
        cells[(vt.rows - 1) * vt.cols + col].fg_color = vt.fg_color;
        cells[(vt.rows - 1) * vt.cols + col].bg_color = vt.bg_color;
      }
      dirty_row_start = 0;
      dirty_row_end = vt.rows;
      display_client_scroll_up(vt.bg_color);
    }
    mark_dirty(vt.cursor_y);
    return;
  }
  if (c == '\r') {
    vt.cursor_x = 0;
    return;
  }
  if (c == '\b') {
    if (vt.cursor_x > 0) {
      vt.cursor_x--;
      struct cell *ce = &cells[vt.cursor_y * vt.cols + vt.cursor_x];
      ce->ch = ' ';
      ce->fg_color = vt.fg_color;
      ce->bg_color = vt.bg_color;
      mark_dirty(vt.cursor_y);
    }
    return;
  }
  if (c == '\t') {
    int next_tab = (vt.cursor_x + 8) & ~7;
    if (next_tab >= vt.cols) {
      vt.cursor_x = 0;
      if (vt.cursor_y < vt.rows - 1)
        vt.cursor_y++;
    } else {
      vt.cursor_x = next_tab;
    }
    return;
  }
  if (vt.cursor_x >= vt.cols) {
    vt.cursor_x = 0;
    if (vt.cursor_y < vt.rows - 1)
      vt.cursor_y++;
    else {
      for (int r = 0; r < vt.rows - 1; r++)
        for (int col = 0; col < vt.cols; col++)
          cells[r * vt.cols + col] = cells[(r + 1) * vt.cols + col];
      for (int col = 0; col < vt.cols; col++) {
        cells[(vt.rows - 1) * vt.cols + col].ch = ' ';
        cells[(vt.rows - 1) * vt.cols + col].fg_color = vt.fg_color;
        cells[(vt.rows - 1) * vt.cols + col].bg_color = vt.bg_color;
      }
      dirty_row_start = 0;
      dirty_row_end = vt.rows;
      display_client_scroll_up(vt.bg_color);
    }
  }
  struct cell *ce = &cells[vt.cursor_y * vt.cols + vt.cursor_x];
  ce->ch = (uint8_t)c;
  ce->fg_color = vt.fg_color;
  ce->bg_color = vt.bg_color;
  mark_dirty(vt.cursor_y);
  vt.cursor_x++;
}

static int csi_param(int idx, int default_val) {
  if (idx < vt.csi_param_count && vt.csi_params[idx] > 0)
    return vt.csi_params[idx];
  return default_val;
}

static void vt100_csi_dispatch(char final_ch) {
  // DEC private modes (?...) only respond to h/l finals here.
  if (vt.csi_private) {
    switch (final_ch) {
    case 'h': // DECSET
      if (csi_param(0, 0) == 25)
        vt.cursor_visible = 1;
      break;
    case 'l': // DECRST
      if (csi_param(0, 0) == 25)
        vt.cursor_visible = 0;
      break;
    }
    return;
  }
  switch (final_ch) {
  case 'A': // CUU
    vt.cursor_y -= csi_param(0, 1);
    if (vt.cursor_y < 0)
      vt.cursor_y = 0;
    break;
  case 'B': // CUD
    vt.cursor_y += csi_param(0, 1);
    if (vt.cursor_y >= vt.rows)
      vt.cursor_y = vt.rows - 1;
    break;
  case 'C': // CUF
    vt.cursor_x += csi_param(0, 1);
    if (vt.cursor_x >= vt.cols)
      vt.cursor_x = vt.cols - 1;
    break;
  case 'D': // CUB
    vt.cursor_x -= csi_param(0, 1);
    if (vt.cursor_x < 0)
      vt.cursor_x = 0;
    break;
  case 'G': // CHA — column (1-based)
    vt.cursor_x = csi_param(0, 1) - 1;
    if (vt.cursor_x < 0)
      vt.cursor_x = 0;
    if (vt.cursor_x >= vt.cols)
      vt.cursor_x = vt.cols - 1;
    break;
  case 'd': // VPA — row (1-based)
    vt.cursor_y = csi_param(0, 1) - 1;
    if (vt.cursor_y < 0)
      vt.cursor_y = 0;
    if (vt.cursor_y >= vt.rows)
      vt.cursor_y = vt.rows - 1;
    break;
  case 's': // SCOSC — save cursor
    vt.saved_x = vt.cursor_x;
    vt.saved_y = vt.cursor_y;
    break;
  case 'u': // SCORC — restore cursor
    vt.cursor_x = vt.saved_x;
    vt.cursor_y = vt.saved_y;
    break;
  case 'H':
  case 'f': {
    int row = csi_param(0, 1) - 1, col = csi_param(1, 1) - 1;
    if (row < 0)
      row = 0;
    if (row >= vt.rows)
      row = vt.rows - 1;
    if (col < 0)
      col = 0;
    if (col >= vt.cols)
      col = vt.cols - 1;
    vt.cursor_y = row;
    vt.cursor_x = col;
    break;
  }
  case 'J': {
    if (csi_param(0, 0) == 2) {
      for (int r = 0; r < vt.rows; r++)
        for (int c = 0; c < vt.cols; c++) {
          cells[r * vt.cols + c].ch = ' ';
          cells[r * vt.cols + c].fg_color = vt.fg_color;
          cells[r * vt.cols + c].bg_color = vt.bg_color;
        }
      dirty_row_start = 0;
      dirty_row_end = vt.rows;
      vt.cursor_x = 0;
      vt.cursor_y = 0;
      display_client_clear(vt.bg_color);
    }
    break;
  }
  case 'K': {
    int mode = csi_param(0, 0);
    int from, to;
    if (mode == 0) { // erase cursor → EOL
      from = vt.cursor_x;
      to = vt.cols;
    } else if (mode == 1) { // erase BOL → cursor
      from = 0;
      to = vt.cursor_x + 1;
    } else { // mode == 2: whole line
      from = 0;
      to = vt.cols;
    }
    for (int c = from; c < to; c++) {
      struct cell *ce = &cells[vt.cursor_y * vt.cols + c];
      ce->ch = ' ';
      ce->fg_color = vt.fg_color;
      ce->bg_color = vt.bg_color;
    }
    mark_dirty(vt.cursor_y);
    break;
  }
  case 'm': {
    if (vt.csi_param_count == 0) {
      vt.fg_color = 0xFFFFFF;
      vt.bg_color = 0x000000;
    } else
      for (int i = 0; i < vt.csi_param_count; i++) {
        int p = vt.csi_params[i];
        if (p == 0) {
          vt.fg_color = 0xFFFFFF;
          vt.bg_color = 0x000000;
        } else if (p >= 30 && p <= 37)
          vt.fg_color = vt100_colors[p - 30];
        else if (p >= 40 && p <= 47)
          vt.bg_color = vt100_colors[p - 40];
        else if (p >= 90 && p <= 97)
          vt.fg_color = vt100_colors[p - 90 + 8];
        else if (p >= 100 && p <= 107)
          vt.bg_color = vt100_colors[p - 100 + 8];
      }
    break;
  }
  default:
    break;
  }
}

static void vt100_feed(char c) {
  switch (vt.escape_state) {
  case VT100_NORMAL:
    if (c == 0x1B)
      vt.escape_state = VT100_ESC;
    else
      cell_putc(c);
    break;
  case VT100_ESC:
    if (c == '[') {
      vt.escape_state = VT100_CSI;
      vt.csi_param_count = 0;
      vt.csi_private = 0;
      for (int i = 0; i < 8; i++)
        vt.csi_params[i] = 0;
    } else if (c == '7') { // DECSC — save cursor
      vt.saved_x = vt.cursor_x;
      vt.saved_y = vt.cursor_y;
      vt.escape_state = VT100_NORMAL;
    } else if (c == '8') { // DECRC — restore cursor
      vt.cursor_x = vt.saved_x;
      vt.cursor_y = vt.saved_y;
      vt.escape_state = VT100_NORMAL;
    } else {
      cell_putc(c);
      vt.escape_state = VT100_NORMAL;
    }
    break;
  case VT100_CSI:
    if (c >= '0' && c <= '9') {
      if (vt.csi_param_count < 8)
        vt.csi_params[vt.csi_param_count] =
            vt.csi_params[vt.csi_param_count] * 10 + (c - '0');
    } else if (c == ';') {
      vt.csi_param_count++;
    } else if (c == '?' && vt.csi_param_count == 0 &&
               vt.csi_params[0] == 0) { // DEC private marker (only at start)
      vt.csi_private = 1;
    } else if (c >= 0x40 && c <= 0x7E) {
      vt.csi_param_count++;
      vt100_csi_dispatch(c);
      vt.escape_state = VT100_NORMAL;
    }
    break;
  }
}

static void draw_cursor_overlay() {
  if (cursor_drawn) {
    // Restore the previously-overlaid cell to its normal colors.
    struct cell *ce = &cells[prev_cy * vt.cols + prev_cx];
    display_client_render_cell(prev_cy, prev_cx, ce->ch, ce->fg_color,
                               ce->bg_color);
    cursor_drawn = 0;
  }
  if (!vt.cursor_visible)
    return;
  int cx = vt.cursor_x, cy = vt.cursor_y;
  if (cx < 0 || cx >= vt.cols || cy < 0 || cy >= vt.rows)
    return;
  struct cell *ce = &cells[cy * vt.cols + cx];
  // Invert fg/bg for a block cursor.
  display_client_render_cell(cy, cx, ce->ch, ce->bg_color, ce->fg_color);
  cursor_drawn = 1;
  prev_cx = cx;
  prev_cy = cy;
}

static void flush_dirty_cells() {
  if (!terminal_seat.active || dirty_row_start >= dirty_row_end) {
    // Even with no dirty cells, the cursor may have moved (e.g. \e[A) — redraw
    // its overlay so the visible cursor tracks the logical one.
    if (terminal_seat.active) {
      draw_cursor_overlay();
      display_client_set_cursor(vt.cursor_x, vt.cursor_y);
    }
    return;
  }
  for (int row = dirty_row_start; row < dirty_row_end; row++)
    for (int col = 0; col < vt.cols; col++) {
      struct cell *c = &cells[row * vt.cols + col];
      display_client_render_cell(row, col, c->ch, c->fg_color, c->bg_color);
    }
  draw_cursor_overlay();
  display_client_set_cursor(vt.cursor_x, vt.cursor_y);
  int rs = dirty_row_start, re = dirty_row_end;
  dirty_row_start = vt.rows;
  dirty_row_end = 0;
  display_client_flush(rs, re);
}

// ===================== Keyboard mapping =====================

struct keymap_entry {
  uint8_t normal;
  uint8_t shifted;
};

static const struct keymap_entry keymap[128] = {
    {0, 0},       {0x1B, 0x1B}, {'1', '!'}, {'2', '@'},   {'3', '#'},
    {'4', '$'},   {'5', '%'},   {'6', '^'}, {'7', '&'},   {'8', '*'},
    {'9', '('},   {'0', ')'},   {'-', '_'}, {'=', '+'},   {'\b', '\b'},
    {'\t', '\t'}, {'q', 'Q'},   {'w', 'W'}, {'e', 'E'},   {'r', 'R'},
    {'t', 'T'},   {'y', 'Y'},   {'u', 'U'}, {'i', 'I'},   {'o', 'O'},
    {'p', 'P'},   {'[', '{'},   {']', '}'}, {'\n', '\n'}, {0, 0},
    {'a', 'A'},   {'s', 'S'},   {'d', 'D'}, {'f', 'F'},   {'g', 'G'},
    {'h', 'H'},   {'j', 'J'},   {'k', 'K'}, {'l', 'L'},   {';', ':'},
    {'\'', '"'},  {'`', '~'},   {0, 0},     {'\\', '|'},  {'z', 'Z'},
    {'x', 'X'},   {'c', 'C'},   {'v', 'V'}, {'b', 'B'},   {'n', 'N'},
    {'m', 'M'},   {',', '<'},   {'.', '>'}, {'/', '?'},   {0, 0},
    {0, 0},       {0, 0},       {' ', ' '}, {0, 0},
};

#define MOD_SHIFT 1
#define MOD_CTRL 2
#define MOD_ALT 4

static int modifiers;

static int key_to_ascii(uint32_t key, int pressed, char *out, int out_max) {
  (void)out_max;
  if (!pressed)
    return 0;

  if (key == 29 || key == 97) {
    if (pressed)
      modifiers |= MOD_CTRL;
    else
      modifiers &= ~MOD_CTRL;
    return 0;
  }
  if (key == 42 || key == 54) {
    if (pressed)
      modifiers |= MOD_SHIFT;
    else
      modifiers &= ~MOD_SHIFT;
    return 0;
  }
  if (key == 56 || key == 100) {
    if (pressed)
      modifiers |= MOD_ALT;
    else
      modifiers &= ~MOD_ALT;
    return 0;
  }
  if (key == 58) {
    if (pressed)
      modifiers ^= 0x10;
    return 0;
  }

  if (key >= 128)
    return 0;

  if (key == 103) {
    out[0] = 0x1B;
    out[1] = '[';
    out[2] = 'A';
    return 3;
  }
  if (key == 108) {
    out[0] = 0x1B;
    out[1] = '[';
    out[2] = 'B';
    return 3;
  }
  if (key == 106) {
    out[0] = 0x1B;
    out[1] = '[';
    out[2] = 'C';
    return 3;
  }
  if (key == 105) {
    out[0] = 0x1B;
    out[1] = '[';
    out[2] = 'D';
    return 3;
  }
  if (key == 102) {
    out[0] = 0x1B;
    out[1] = '[';
    out[2] = 'H';
    return 3;
  }
  if (key == 107) {
    out[0] = 0x1B;
    out[1] = '[';
    out[2] = 'F';
    return 3;
  }
  if (key == 14) {
    out[0] = '\b';
    return 1;
  }
  if (key == 15) {
    out[0] = '\t';
    return 1;
  }

  int shift = (modifiers & MOD_SHIFT) ? 1 : 0;
  if (modifiers & 0x10)
    shift = !shift;
  const struct keymap_entry *me = &keymap[key];
  if (me->normal == 0)
    return 0;
  char ch = shift ? me->shifted : me->normal;
  if (modifiers & MOD_CTRL) {
    if (ch >= 'a' && ch <= 'z')
      ch = ch - 'a' + 1;
    else if (ch >= 'A' && ch <= 'Z')
      ch = ch - 'A' + 1;
    if (ch == '[')
      ch = 0x1B;
  }
  out[0] = ch;
  return 1;
}

// ===================== libinput interface =====================

static int open_seat_drm(struct terminal_seat *session) {
  session->drm_id =
      libseat_open_device(session->seat, "/dev/dri/card0", &session->drm_fd);
  if (session->drm_id <= 0 || session->drm_fd < 0)
    return -1;
  if (display_client_init(session->drm_fd) == 0)
    return 0;

  (void)libseat_close_device(session->seat, session->drm_id);
  close(session->drm_fd);
  session->drm_id = -1;
  session->drm_fd = -1;
  return -1;
}

static void close_seat_drm(struct terminal_seat *session) {
  display_client_destroy();
  if (session->drm_id > 0)
    (void)libseat_close_device(session->seat, session->drm_id);
  if (session->drm_fd >= 0)
    close(session->drm_fd);
  session->drm_id = -1;
  session->drm_fd = -1;
}

static void enable_seat(struct libseat *seat, void *data) {
  (void)seat;
  struct terminal_seat *session = (struct terminal_seat *)data;
  if (session->active) {
    session->fatal = 1;
    return;
  }
  session->active = 1;
  if (!session->initialized)
    return;

  if (open_seat_drm(session) < 0 || libinput_resume(session->libinput) < 0) {
    session->fatal = 1;
    return;
  }
  dirty_row_start = 0;
  dirty_row_end = vt.rows;
  flush_dirty_cells();
}

static void disable_seat(struct libseat *seat, void *data) {
  struct terminal_seat *session = (struct terminal_seat *)data;
  if (!session->active) {
    session->fatal = 1;
    return;
  }
  if (session->libinput != NULL)
    libinput_suspend(session->libinput);
  close_seat_drm(session);
  session->active = 0;
  if (libseat_disable_seat(seat) < 0)
    session->fatal = 1;
}

static const struct libseat_seat_listener seat_listener = {
    .enable_seat = enable_seat,
    .disable_seat = disable_seat,
};

// libinput log handler
#include <stdarg.h>
static void libinput_log(struct libinput *libinput,
                         enum libinput_log_priority prio, const char *format,
                         va_list args) {
  (void)libinput;
  char buf[256];
  int n = vsnprintf(buf, sizeof(buf), format, args);
  if (n > 0)
    fprintf(stderr, "LIBINPUT(%d): %s\n", prio, buf);
}

static int open_restricted(const char *path, int flags, void *user_data) {
  (void)flags;
  struct terminal_seat *session = (struct terminal_seat *)user_data;
  int fd = -1;
  int id = libseat_open_device(session->seat, path, &fd);
  if (id < 0)
    return -errno;
  for (size_t i = 0; i < MAX_SEAT_INPUT_DEVICES; i++) {
    if (session->inputs[i].id == 0) {
      session->inputs[i].fd = fd;
      session->inputs[i].id = id;
      return fd;
    }
  }
  (void)libseat_close_device(session->seat, id);
  close(fd);
  return -EMFILE;
}

static void close_restricted(int fd, void *user_data) {
  struct terminal_seat *session = (struct terminal_seat *)user_data;
  for (size_t i = 0; i < MAX_SEAT_INPUT_DEVICES; i++) {
    if (session->inputs[i].id != 0 && session->inputs[i].fd == fd) {
      (void)libseat_close_device(session->seat, session->inputs[i].id);
      session->inputs[i].fd = -1;
      session->inputs[i].id = 0;
      break;
    }
  }
  close(fd);
}

static const struct libinput_interface interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

// ===================== monitor reconnect (M4) =====================

// On detecting monitor death via spin: suspend clears the dead monitor, then
// back off with 6×sleep(1) before resume rebuilds it (§3.3). Resume failure
// is retryable: enable_receiving's failure path cleans up so udev_monitor==NULL
// (the :259 short-circuit guard doesn't fire).
static int resume_reconnect(struct libinput *li) {
  libinput_suspend(
      li); // udev_input_disable: unref + NULL + remove epoll source
  for (int attempt = 0; attempt < 6;
       attempt++) {   // covers init's START_LIMIT_BURST=5 ~5s window + 1 margin
    write(2, "R", 1); // single-char progress: rebuild attempt
    if (libinput_resume(li) == 0)
      return 0; // resume ok: monitor rebuilt + udev_input_add_devices reopens
                // devices
    sleep(1);   // udevd restart window leaves socket name hollow; back off
                // (matches init RESTART_SEC=1)
  }
  return -1; // 6 attempts exhausted → enter_degraded_hold
}

// 6 attempts exhausted (§3.4): stop the tight retry, keep the pty/shell alive
// (session preserved), input dropped, low-frequency 5s probing for self-heal.
// This bridges the gap between this OS having no systemd to keep udevd alive
// and Linux's "udevd comes back and naturally recovers" semantics. During
// probing, keep draining master_fd (pty output) and rendering — otherwise the
// pty buffer fills and back-pressures the shell into write-blocking, which
// contradicts "shell alive / session preserved" (§3.4 design intent).
// master_fd is O_NONBLOCK (read returns -1/EAGAIN when no data), so draining
// doesn't block the probe cadence.
static void enter_degraded_hold(struct libinput *li) {
  write(2, "D", 1); // single-char progress: degraded
  fprintf(stderr, "terminal: monitor dead, input degraded\n");
  while (1) {
    sleep(5); // low-frequency probe
    if (libinput_resume(li) == 0)
      return; // probe succeeded: monitor rebuilt, return to main loop, restore
              // input
    // drain pty output to keep the shell alive (session preserved)
    char b[4096];
    int64_t n;
    while ((n = read(master_fd, b, sizeof(b))) > 0) {
      for (int64_t i = 0; i < n; i++)
        vt100_feed(b[i]);
      flush_dirty_cells();
    }
  }
}

// ===================== Main =====================

// spawn_shell: create a PTY pair via the standard forkpty path (setsid +
// TIOCSCTTY + dup2(slave,0/1/2) are done by libc login_tty in the child) and
// exec the shell. On success returns the child pid with master_fd installed
// (O_NONBLOCK); on failure returns -1. Exec failure is reported on the slave
// so it appears on the terminal instead of a silent 127 (step1.md §3.2).
static pid_t spawn_shell(const struct winsize *ws) {
  int master;
  pid_t pid = forkpty(&master, NULL, NULL, ws);
  if (pid < 0)
    return -1;
  if (pid == 0) {
    setenv("TERM", "xterm-256color", 1);
    execlp("/bin/sh", "/bin/sh", (char *)NULL);
    char msg[64];
    int len =
        snprintf(msg, sizeof(msg), "exec: /bin/sh: %s\n", strerror(errno));
    if (len > 0)
      write(1, msg, (size_t)len);
    _exit(127);
  }
  fcntl(master, F_SETFL, O_RDWR | O_NONBLOCK);
  master_fd = master;
  return pid;
}

// extern "C": clang under -ffreestanding mangles a C++ `main`, breaking the
// crt0.o `main` reference; gcc leaves `main` unmangled regardless. See
// shell.cc.
extern "C" int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;

  memset(&terminal_seat, 0, sizeof(terminal_seat));
  terminal_seat.drm_fd = -1;
  terminal_seat.drm_id = -1;
  for (size_t i = 0; i < MAX_SEAT_INPUT_DEVICES; i++)
    terminal_seat.inputs[i].fd = -1;

  terminal_seat.seat = libseat_open_seat(&seat_listener, &terminal_seat);
  if (terminal_seat.seat == NULL) {
    printf("terminal: libseat_open_seat FAILED: %s\n", strerror(errno));
    return 1;
  }
  while (!terminal_seat.active && !terminal_seat.fatal) {
    if (libseat_dispatch(terminal_seat.seat, -1) < 0)
      terminal_seat.fatal = 1;
  }
  if (terminal_seat.fatal || open_seat_drm(&terminal_seat) < 0) {
    printf("terminal: seatd DRM setup FAILED: %s\n", strerror(errno));
    return 1;
  }

  // Create libinput context (udev backend): udev_new + udev_create_context +
  // assign_seat("seat0"). assign_seat → udev_input_enable builds the udevd
  // monitor + a first-round enumerate reading /sys to pick up existing devices
  // (§2.1/§2.4).
  // udevd not started / crashed → assign_seat fails → black screen (intentional
  // signal, no fallback to the path backend, §2.4).
  struct udev *udev = udev_new();
  struct libinput *li = NULL;
  while (!li) {
    li = libinput_udev_create_context(&interface, &terminal_seat, udev);
    if (!li) {
      struct recv_msg m;
      ipc_recv(&m, NULL, 0, 1);
    }
  }
  terminal_seat.libinput = li;

  // Set up libinput logging (after ctx created)
  libinput_log_set_handler(li, libinput_log);
  libinput_log_set_priority(li, LIBINPUT_LOG_PRIORITY_DEBUG);

  // assign_seat triggers the first-round enumerate + builds the monitor
  // (replacing explicit path_add_device). Device add/remove events are handled
  // automatically by the udev backend; the terminal need not handle
  // ADDED/REMOVED (§2.3).
  if (libinput_udev_assign_seat(li, "seat0") != 0) {
    printf("terminal: libinput_udev_assign_seat FAILED (udevd down?)\n");
    while (1) {
      struct recv_msg m;
      ipc_recv(&m, NULL, 0, 0);
    }
  }

  int li_fd = libinput_get_fd(li);

  vt.cols = display_cols;
  vt.rows = display_rows;
  vt.cursor_x = 0;
  vt.cursor_y = 0;
  vt.fg_color = 0xFFFFFF;
  vt.bg_color = 0x000000;
  vt.escape_state = VT100_NORMAL;
  vt.csi_param_count = 0;
  vt.csi_private = 0;
  vt.saved_x = 0;
  vt.saved_y = 0;
  vt.cursor_visible = 1;
  cursor_drawn = 0;

  int cell_bytes = vt.rows * vt.cols * sizeof(struct cell);
  cells =
      (struct cell *)mmap(NULL, cell_bytes, PROT_READ | PROT_WRITE, 0, -1, 0);
  if (!cells) {
    printf("terminal: mmap cells FAILED\n");
    while (1) {
      struct recv_msg m;
      ipc_recv(&m, NULL, 0, 0);
    }
  }

  for (int r = 0; r < vt.rows; r++)
    for (int c = 0; c < vt.cols; c++) {
      cells[r * vt.cols + c].ch = ' ';
      cells[r * vt.cols + c].fg_color = 0xFFFFFF;
      cells[r * vt.cols + c].bg_color = 0x000000;
    }
  dirty_row_start = vt.rows;
  dirty_row_end = 0;
  display_client_clear(0x000000);
  display_client_flush(0, display_rows);
  terminal_seat.initialized = 1;

  struct winsize ws;
  ws.ws_row = display_rows;
  ws.ws_col = display_cols;
  ws.ws_xpixel = 0;
  ws.ws_ypixel = 0;

  shell_pid = spawn_shell(&ws);
  if (shell_pid < 0) {
    printf("terminal: forkpty failed\n");
    return 1;
  }

  // monitor death detection: count of consecutive "immediate return with zero
  // real events" (§3.2). EOF busy-spin is the only observable side effect of a
  // permanently-readable pipe rd under level-triggered epoll (§3.1).
  int spin_count = 0;
  const int SPIN_THRESHOLD = 2000;

  while (1) {
    if (libseat_dispatch(terminal_seat.seat, 0) < 0 || terminal_seat.fatal) {
      printf("terminal: libseat session failed: %s\n", strerror(errno));
      return 1;
    }
    if (!terminal_seat.active) {
      struct pollfd seat_pfd = {.fd = libseat_get_fd(terminal_seat.seat),
                                .events = POLLIN,
                                .revents = 0};
      if (poll(&seat_pfd, 1, -1) < 0 && errno != EINTR)
        return 1;
      continue;
    }

    // Drain all pending keyboard events via libinput (non-blocking dispatch).
    // Line editing, echo and signal characters are handled by the kernel PTY
    // line discipline (step1.md §3.4); every byte is forwarded as-is.
    libinput_dispatch(li);
    struct libinput_event *lev;
    int got_event = 0;
    while ((lev = libinput_get_event(li)) != NULL) {
      got_event = 1;
      if (libinput_event_get_type(lev) == LIBINPUT_EVENT_KEYBOARD_KEY) {
        struct libinput_event_keyboard *kbev =
            libinput_event_get_keyboard_event(lev);
        uint32_t key = libinput_event_keyboard_get_key(kbev);
        enum libinput_key_state state =
            libinput_event_keyboard_get_key_state(kbev);

        char ascii_buf[4];
        int ascii_len = key_to_ascii(key, state == LIBINPUT_KEY_STATE_PRESSED,
                                     ascii_buf, sizeof(ascii_buf));
        if (ascii_len > 0)
          write(master_fd, ascii_buf, (size_t)ascii_len);
      }
      libinput_event_destroy(lev);
    }

    // Shell output
    char buf[4096];
    int64_t n = read(master_fd, buf, sizeof(buf));
    if (n > 0) {
      got_event = 1; // pty data counts as real activity; reset spin
      for (int64_t i = 0; i < n; i++) {
        vt100_feed(buf[i]);
        if (dirty_row_end - dirty_row_start >= 4)
          flush_dirty_cells();
      }
    } else if (n == 0) {
      printf("terminal: shell exited (n=0), re-forking\n");
      close(master_fd);
      master_fd = -1;
      shell_pid = spawn_shell(&ws);
      if (shell_pid < 0)
        continue;
    }

    flush_dirty_cells();

    // Poll on seat lifecycle, libinput, and shell output.
    struct pollfd pfds[3];
    pfds[0].fd = libseat_get_fd(terminal_seat.seat);
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = li_fd;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    pfds[2].fd = master_fd;
    pfds[2].events = POLLIN;
    pfds[2].revents = 0;
    int pr = poll(pfds, 3, -1);

    // spin detection: li_fd permanently readable but never yielding events =
    // monitor pipe EOF busy-spin (§3.2). Real events (key/pty) reset it; only
    // EOF busy-spin accumulates to the threshold. master_fd is reassembled
    // each round after pty rebuild, orthogonal to resume (§3.3, grill Q6).
    int real_activity = got_event || (pfds[2].revents & POLLIN);
    if (!real_activity && pr > 0 && (pfds[1].revents & POLLIN)) {
      if (++spin_count >= SPIN_THRESHOLD) {
        spin_count = 0;
        if (resume_reconnect(li) != 0) // path B: suspend→resume rebuild
          enter_degraded_hold(li);     // path C: degraded hold + 5s probing
      }
    } else {
      spin_count = 0;
    }
  }
  return 0;
}
