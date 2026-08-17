/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Real terminal rendered with Vulkan WSI (Wayland client).
//   · forkpty() spawns $SHELL: keyboard input is written to the PTY, shell
//     output is read back and parsed
//   · Built-in minimal VT emulator: UTF-8, CSI
//   (cursor/clear/scroll/insert-delete),
//     SGR 16/256/truecolor, alternate screen (less/htop work), OSC title
//   · Text is rasterized by freetype into a glyph atlas and drawn directly by
//   Vulkan · Window decorations are drawn uniformly by the compositor using SSD
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h> // BTN_LEFT
#include <math.h>
#include <poll.h>
#include <pty.h> // forkpty
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xos/perf.h>
#include <xos/syscall_nums.h>

#include <ft2build.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include FT_FREETYPE_H

#ifdef TERMINAL_SGR_TEST
#include <unity.h>
#endif

#include "xdg-decoration-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#ifndef TERMINAL_SGR_TEST
#include "terminal_vulkan.h"
#endif

#define FONT_SIZE 14.0
#define TITLEBAR_H 30.0
#define CORNER_RADIUS 10.0
#define TERM_PAD 12.0

#define MAX_COLS 512
#define MAX_ROWS 512

// Colors are stored as 0xRRGGBB; COLOR_DEFAULT means "use the default color"
// (fg uses light-green-white, bg is not filled)
#define COLOR_DEFAULT UINT32_MAX

#define CELL_BOLD (1u << 0)
#define CELL_UNDERLINE (1u << 1)
#define CELL_INVERSE (1u << 2)
#define CELL_STRIKE (1u << 3)

enum cursor_shape { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BAR };

// RGB values occupy bits 0-23. Indexed colors retain their palette identity so
// bold can brighten ANSI colors at render time, independent of SGR order.
#define COLOR_INDEXED (1u << 24)
#define COLOR_INDEX(i) (COLOR_INDEXED | (uint32_t)(i))

// ---------------------------------------------------------------------------
// Terminal emulator: cell grid + ANSI escape sequence parsing
// ---------------------------------------------------------------------------
struct cell {
  uint32_t cp;     // Unicode codepoint (0 = empty)
  uint32_t fg, bg; // 0xRRGGBB, COLOR_INDEX(n), or COLOR_DEFAULT
  uint8_t flags;   // CELL_*
};

enum { P_GROUND, P_ESC, P_ESC_SKIP, P_CSI, P_OSC };

struct term {
  int cols, rows;
  struct cell *grid; // main screen
  struct cell *alt;  // alternate screen (ESC[?1049h, used by less/htop/vim)
  bool alt_active;
  int cx, cy;
  int saved_cx, saved_cy;
  int scroll_top, scroll_bottom;
  uint32_t fg, bg;
  bool bold, underline, inverse, strike;
  bool wrap_pending; // cursor sits at the last column, next char wraps first
  bool cursor_visible;
  bool origin_mode;        // DECOM: row coordinates relative to scroll region,
                           // movement restricted to it
  bool application_cursor; // DECCKM: arrow keys send SS3 sequences
  enum cursor_shape cursor_shape;

  // Parser state
  int pstate;
  int params[16];
  int nparams;
  char prefix;       // CSI private prefix (currently handles '?')
  char intermediate; // CSI intermediate byte (e.g. the space in DECSCUSR)
  char osc[256];
  int osc_len;
  uint32_t utf8_cp; // incomplete UTF-8 sequence
  int utf8_need;
};

// xterm 256-color palette: 0-15 base colors, 16-231 the 6x6x6 cube, 232-255
// grayscale
static uint32_t palette[256];

static void palette_init(void) {
  static const uint32_t base[16] = {
      0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd,
      0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
      0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
  };
  memcpy(palette, base, sizeof(base));
  static const int lvl[6] = {0, 95, 135, 175, 215, 255};
  for (int i = 0; i < 216; i++)
    palette[16 + i] =
        (lvl[i / 36] << 16) | (lvl[(i / 6) % 6] << 8) | lvl[i % 6];
  for (int i = 0; i < 24; i++) {
    uint32_t g = 8 + 10 * i;
    palette[232 + i] = (g << 16) | (g << 8) | g;
  }
}

static uint32_t resolve_color(uint32_t color, bool bold) {
  if (color == COLOR_DEFAULT)
    return color;
  if (color & COLOR_INDEXED) {
    unsigned int index = color & 0xff;
    if (bold && index < 8)
      index += 8;
    return palette[index];
  }
  return color;
}

static struct term term;

static void term_reply(const char *s, size_t n);

static struct cell *cur_grid(void) {
  return term.alt_active ? term.alt : term.grid;
}

static struct cell *cell_at(int x, int y) {
  return &cur_grid()[y * term.cols + x];
}

static struct cell blank_cell(void) {
  // Erase with the current SGR background color (xterm behavior, so colored
  // screensavers/progress bars render correctly)
  return (struct cell){.cp = 0, .fg = COLOR_DEFAULT, .bg = term.bg, .flags = 0};
}

static void clear_row(int y, int x0, int x1) {
  struct cell b = blank_cell();
  for (int x = x0; x <= x1; x++)
    *cell_at(x, y) = b;
}

static void clear_rows(int y0, int y1) {
  for (int y = y0; y <= y1; y++)
    clear_row(y, 0, term.cols - 1);
}

static void scroll_up(int top, int bottom, int n) {
  if (n > bottom - top + 1)
    n = bottom - top + 1;
  struct cell *g = cur_grid();
  memmove(&g[top * term.cols], &g[(top + n) * term.cols],
          (size_t)(bottom - top + 1 - n) * term.cols * sizeof(struct cell));
  clear_rows(bottom - n + 1, bottom);
}

static void scroll_down(int top, int bottom, int n) {
  if (n > bottom - top + 1)
    n = bottom - top + 1;
  struct cell *g = cur_grid();
  memmove(&g[(top + n) * term.cols], &g[top * term.cols],
          (size_t)(bottom - top + 1 - n) * term.cols * sizeof(struct cell));
  clear_rows(top, top + n - 1);
}

static void newline(void) {
  term.wrap_pending = false;
  if (term.cy == term.scroll_bottom)
    scroll_up(term.scroll_top, term.scroll_bottom, 1);
  else if (term.cy < term.rows - 1)
    term.cy++;
}

static void put_char(uint32_t cp) {
  if (term.wrap_pending) {
    term.cx = 0;
    newline();
  }
  struct cell *c = cell_at(term.cx, term.cy);
  c->cp = cp;
  c->fg = term.fg;
  c->bg = term.bg;
  c->flags =
      (term.bold ? CELL_BOLD : 0) | (term.underline ? CELL_UNDERLINE : 0) |
      (term.inverse ? CELL_INVERSE : 0) | (term.strike ? CELL_STRIKE : 0);
  if (term.cx == term.cols - 1)
    term.wrap_pending = true;
  else
    term.cx++;
}

// ---------------------------------------------------------------------------
// SGR: glyph attributes and colors
// ---------------------------------------------------------------------------
static int color_component(int value) {
  if (value < 0)
    return 0;
  return value > 255 ? 255 : value;
}

static void reset_sgr(void) {
  term.fg = COLOR_DEFAULT;
  term.bg = COLOR_DEFAULT;
  term.bold = false;
  term.underline = false;
  term.inverse = false;
  term.strike = false;
}

static void term_sgr(void) {
  for (int i = 0; i < term.nparams; i++) {
    int p = term.params[i];
    if (p == 0) {
      reset_sgr();
    } else if (p == 1) {
      term.bold = true;
    } else if (p == 4 || p == 21) {
      term.underline = true;
    } else if (p == 22) {
      term.bold = false;
    } else if (p == 24) {
      term.underline = false;
    } else if (p == 7) {
      term.inverse = true;
    } else if (p == 27) {
      term.inverse = false;
    } else if (p == 9) {
      term.strike = true;
    } else if (p == 29) {
      term.strike = false;
    } else if (p == 39) {
      term.fg = COLOR_DEFAULT;
    } else if (p == 49) {
      term.bg = COLOR_DEFAULT;
    } else if (p >= 30 && p <= 37) {
      term.fg = COLOR_INDEX(p - 30);
    } else if (p >= 40 && p <= 47) {
      term.bg = COLOR_INDEX(p - 40);
    } else if (p >= 90 && p <= 97) {
      term.fg = COLOR_INDEX(p - 90 + 8);
    } else if (p >= 100 && p <= 107) {
      term.bg = COLOR_INDEX(p - 100 + 8);
    } else if ((p == 38 || p == 48) && i + 2 < term.nparams &&
               term.params[i + 1] == 5) {
      uint32_t col = COLOR_INDEX(color_component(term.params[i + 2]));
      if (p == 38)
        term.fg = col;
      else
        term.bg = col;
      i += 2;
    } else if ((p == 38 || p == 48) && i + 4 < term.nparams &&
               term.params[i + 1] == 2) {
      uint32_t col = ((uint32_t)color_component(term.params[i + 2]) << 16) |
                     ((uint32_t)color_component(term.params[i + 3]) << 8) |
                     (uint32_t)color_component(term.params[i + 4]);
      if (p == 38)
        term.fg = col;
      else
        term.bg = col;
      i += 4;
    }
  }
}

// ---------------------------------------------------------------------------
// CSI dispatch. PP(i, def): the i-th parameter, defaults to def when absent/0
// ---------------------------------------------------------------------------
#define PP(i, def)                                                             \
  ((i) < term.nparams && term.params[(i)] > 0 ? term.params[(i)] : (def))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

static int cursor_top(void) { return term.origin_mode ? term.scroll_top : 0; }

static int cursor_bottom(void) {
  return term.origin_mode ? term.scroll_bottom : term.rows - 1;
}

static void cursor_home(void) {
  term.cx = 0;
  term.cy = cursor_top();
  term.wrap_pending = false;
}

static void term_csi(uint8_t f) {
  int n;
  switch (f) {
  case 'A': // cursor up
    term.cy = CLAMP(term.cy - PP(0, 1), cursor_top(), cursor_bottom());
    break;
  case 'B':
  case 'e': // cursor down
    term.cy = CLAMP(term.cy + PP(0, 1), cursor_top(), cursor_bottom());
    break;
  case 'C':
  case 'a': // cursor right
    term.cx = CLAMP(term.cx + PP(0, 1), 0, term.cols - 1);
    term.wrap_pending = false;
    break;
  case 'D': // cursor left
    term.cx = CLAMP(term.cx - PP(0, 1), 0, term.cols - 1);
    term.wrap_pending = false;
    break;
  case 'E':
    term.cy = CLAMP(term.cy + PP(0, 1), cursor_top(), cursor_bottom());
    term.cx = 0;
    term.wrap_pending = false;
    break;
  case 'F':
    term.cy = CLAMP(term.cy - PP(0, 1), cursor_top(), cursor_bottom());
    term.cx = 0;
    term.wrap_pending = false;
    break;
  case 'G':
  case '`': // move to column
    term.cx = CLAMP(PP(0, 1) - 1, 0, term.cols - 1);
    term.wrap_pending = false;
    break;
  case 'd': // move to row
    term.cy = CLAMP(cursor_top() + PP(0, 1) - 1, cursor_top(), cursor_bottom());
    break;
  case 'H':
  case 'f': // move to (row,column)
    term.cy = CLAMP(cursor_top() + PP(0, 1) - 1, cursor_top(), cursor_bottom());
    term.cx = CLAMP(PP(1, 1) - 1, 0, term.cols - 1);
    term.wrap_pending = false;
    break;
  case 'J': // clear screen
    n = PP(0, 0);
    if (n == 0) {
      clear_row(term.cy, term.cx, term.cols - 1);
      clear_rows(term.cy + 1, term.rows - 1);
    } else if (n == 1) {
      clear_rows(0, term.cy - 1);
      clear_row(term.cy, 0, term.cx);
    } else {
      clear_rows(0, term.rows - 1);
    }
    break;
  case 'K': // clear line
    n = PP(0, 0);
    if (n == 0)
      clear_row(term.cy, term.cx, term.cols - 1);
    else if (n == 1)
      clear_row(term.cy, 0, term.cx);
    else
      clear_row(term.cy, 0, term.cols - 1);
    break;
  case 'L': // insert lines
    scroll_down(term.cy, term.scroll_bottom, PP(0, 1));
    term.cx = 0;
    term.wrap_pending = false;
    break;
  case 'M': // delete lines
    scroll_up(term.cy, term.scroll_bottom, PP(0, 1));
    term.cx = 0;
    term.wrap_pending = false;
    break;
  case 'P': { // delete characters
    n = PP(0, 1);
    if (n > term.cols - term.cx)
      n = term.cols - term.cx;
    struct cell *row = &cur_grid()[term.cy * term.cols];
    memmove(&row[term.cx], &row[term.cx + n],
            (size_t)(term.cols - term.cx - n) * sizeof(struct cell));
    clear_row(term.cy, term.cols - n, term.cols - 1);
    break;
  }
  case '@': { // insert blank characters
    n = PP(0, 1);
    if (n > term.cols - term.cx)
      n = term.cols - term.cx;
    struct cell *row = &cur_grid()[term.cy * term.cols];
    memmove(&row[term.cx + n], &row[term.cx],
            (size_t)(term.cols - term.cx - n) * sizeof(struct cell));
    clear_row(term.cy, term.cx, term.cx + n - 1);
    break;
  }
  case 'X': // erase characters (without shifting the following content)
    n = CLAMP(PP(0, 1), 0, term.cols - term.cx);
    clear_row(term.cy, term.cx, term.cx + n - 1);
    break;
  case 'S': // scroll up
    scroll_up(term.scroll_top, term.scroll_bottom, PP(0, 1));
    break;
  case 'T': // scroll down
    scroll_down(term.scroll_top, term.scroll_bottom, PP(0, 1));
    break;
  case 'm':
    term_sgr();
    break;
  case 'r': { // set scroll region
    int top = PP(0, 1) - 1, bottom = PP(1, term.rows) - 1;
    top = CLAMP(top, 0, term.rows - 1);
    bottom = CLAMP(bottom, 0, term.rows - 1);
    if (top < bottom) {
      term.scroll_top = top;
      term.scroll_bottom = bottom;
    }
    cursor_home();
    break;
  }
  case 'n': { // DSR: device status, cursor position
    if (term.prefix != '\0' && term.prefix != '?')
      break;
    int p = PP(0, 0);
    if (p == 5) {
      term_reply(term.prefix == '?' ? "\033[?0n" : "\033[0n",
                 term.prefix == '?' ? 5 : 4);
    } else if (p == 6) {
      char response[48];
      int row = term.cy - (term.origin_mode ? term.scroll_top : 0) + 1;
      int len = snprintf(response, sizeof(response),
                         term.prefix == '?' ? "\033[?%d;%dR" : "\033[%d;%dR",
                         row, term.cx + 1);
      if (len > 0)
        term_reply(response, (size_t)len);
    }
    break;
  }
  case 'c': // DA: primary device attributes and common secondary attrs query
    if (term.prefix == '\0' && PP(0, 0) == 0)
      term_reply("\033[?1;2c", 7);
    else if (term.prefix == '>' && PP(0, 0) == 0)
      term_reply("\033[>0;1;0c", 9);
    break;
  case 'q': { // DECSCUSR: blinking currently shows the matching static shape
    if (term.intermediate != ' ')
      break;
    int shape = PP(0, 0);
    if (shape <= 2)
      term.cursor_shape = CURSOR_BLOCK;
    else if (shape <= 4)
      term.cursor_shape = CURSOR_UNDERLINE;
    else if (shape <= 6)
      term.cursor_shape = CURSOR_BAR;
    break;
  }
  case 's': // save cursor
    term.saved_cx = term.cx;
    term.saved_cy = term.cy;
    break;
  case 'u': // restore cursor
    term.cx = term.saved_cx;
    term.cy = term.saved_cy;
    term.wrap_pending = false;
    break;
  case 'h':
  case 'l': { // mode set/reset (only handles private '?' modes)
    if (term.prefix != '?')
      break;
    bool on = (f == 'h');
    for (int i = 0; i < term.nparams; i++) {
      switch (term.params[i]) {
      case 1:
        term.application_cursor = on;
        break;
      case 6:
        term.origin_mode = on;
        cursor_home();
        break;
      case 25:
        term.cursor_visible = on;
        break;
      case 1049:
        if (on) {
          term.saved_cx = term.cx;
          term.saved_cy = term.cy;
        } else {
          term.cx = term.saved_cx;
          term.cy = term.saved_cy;
        }
        // fall through
      case 1047:
      case 47:
        term.alt_active = on;
        if (on)
          clear_rows(0, term.rows - 1);
        term.cx = term.cy = 0;
        term.wrap_pending = false;
        break;
      }
    }
    break;
  }
  }
}

// ---------------------------------------------------------------------------
// OSC: only recognizes 0/1/2 (set window title), the rest is discarded
// ---------------------------------------------------------------------------
#ifdef TERMINAL_SGR_TEST
static void osc_done(void) { term.osc[term.osc_len] = '\0'; }
#else
static void osc_done(void);
#endif

static void term_control(uint8_t b) {
  switch (b) {
  case '\r':
    term.cx = 0;
    term.wrap_pending = false;
    break;
  case '\n':
  case '\v':
  case '\f':
    newline();
    break;
  case '\b':
    if (term.cx > 0)
      term.cx--;
    term.wrap_pending = false;
    break;
  case '\t':
    term.cx = CLAMP((term.cx + 8) & ~7, 0, term.cols - 1);
    term.wrap_pending = false;
    break;
  }
  // BEL etc. are simply ignored in the GROUND state
}

// Feed a segment of PTY output bytes to the emulator (may be an incomplete
// UTF-8/escape sequence; parser state persists)
static void term_feed(const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t b = d[i];

    // First complete any pending UTF-8 sequence (C0 control chars < 0x80
    // cannot collide)
    if (term.utf8_need > 0) {
      if ((b & 0xC0) == 0x80) {
        term.utf8_cp = (term.utf8_cp << 6) | (b & 0x3F);
        if (--term.utf8_need == 0)
          put_char(term.utf8_cp);
        continue;
      }
      term.utf8_need =
          0; // invalid sequence: drop it, reprocess b as a normal byte
    }
    if (b >= 0x80 && term.pstate == P_GROUND) {
      if (b >= 0xF0) {
        term.utf8_cp = b & 0x07;
        term.utf8_need = 3;
      } else if (b >= 0xE0) {
        term.utf8_cp = b & 0x0F;
        term.utf8_need = 2;
      } else if (b >= 0xC2) {
        term.utf8_cp = b & 0x1F;
        term.utf8_need = 1;
      }
      // 0x80-0xC1 is an invalid starting byte, drop it
      continue;
    }

    switch (term.pstate) {
    case P_GROUND:
      if (b == 0x1b)
        term.pstate = P_ESC;
      else if (b < 0x20 || b == 0x7f)
        term_control(b);
      else
        put_char(b);
      break;
    case P_ESC:
      term.pstate = P_GROUND;
      switch (b) {
      case '[':
        term.pstate = P_CSI;
        term.nparams = 1;
        term.params[0] = 0;
        term.prefix = '\0';
        term.intermediate = '\0';
        break;
      case ']':
        term.pstate = P_OSC;
        term.osc_len = 0;
        break;
      case '(':
      case ')':
      case '#': // charset/DECALN: swallow the next byte
        term.pstate = P_ESC_SKIP;
        break;
      case '7': // save cursor
        term.saved_cx = term.cx;
        term.saved_cy = term.cy;
        break;
      case '8': // restore cursor
        term.cx = term.saved_cx;
        term.cy = term.saved_cy;
        term.wrap_pending = false;
        break;
      case 'D': // IND
        newline();
        break;
      case 'M': // RI: reverse index (scroll down)
        if (term.cy == term.scroll_top)
          scroll_down(term.scroll_top, term.scroll_bottom, 1);
        else if (term.cy > 0)
          term.cy--;
        term.wrap_pending = false;
        break;
      case 'E': // NEL
        term.cx = 0;
        newline();
        break;
      case 'c': { // RIS: full reset
        term.cx = term.cy = 0;
        reset_sgr();
        term.scroll_top = 0;
        term.scroll_bottom = term.rows - 1;
        term.origin_mode = false;
        term.application_cursor = false;
        term.cursor_visible = true;
        term.cursor_shape = CURSOR_BLOCK;
        clear_rows(0, term.rows - 1);
        break;
      }
      }
      break;
    case P_ESC_SKIP:
      term.pstate = P_GROUND;
      break;
    case P_CSI:
      if (b >= '0' && b <= '9') {
        term.params[term.nparams - 1] =
            term.params[term.nparams - 1] * 10 + (b - '0');
      } else if (b == ';') {
        if (term.nparams < 16)
          term.params[term.nparams++] = 0;
      } else if ((b == '?' || b == '>') && term.nparams == 1 &&
                 term.params[0] == 0) {
        term.prefix = (char)b;
      } else if (b >= 0x20 && b <= 0x2f) {
        term.intermediate = (char)b;
      } else if (b >= 0x40 && b <= 0x7e) {
        term_csi(b);
        term.pstate = P_GROUND;
      }
      // Other intermediate bytes (space, !, etc.) are ignored
      break;
    case P_OSC:
      if (b == 0x07) { // BEL terminates
        osc_done();
        term.pstate = P_GROUND;
      } else if (b ==
                 0x1b) { // ESC \ terminates: return to ESC state, swallow '\'
        osc_done();
        term.pstate = P_ESC;
      } else if (term.osc_len < (int)sizeof(term.osc) - 1) {
        term.osc[term.osc_len++] = (char)b;
      }
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Terminal dimensions
// ---------------------------------------------------------------------------
static void term_init(int cols, int rows) {
  term.cols = cols;
  term.rows = rows;
  term.grid = static_cast<struct cell *>(
      calloc((size_t)cols * rows, sizeof(struct cell)));
  term.alt = static_cast<struct cell *>(
      calloc((size_t)cols * rows, sizeof(struct cell)));
  reset_sgr();
  term.scroll_top = 0;
  term.scroll_bottom = rows - 1;
  term.cursor_visible = true;
  term.cursor_shape = CURSOR_BLOCK;
  clear_rows(0, rows - 1);
}

// Rebuild the grid when the window size changes (preserving main-screen
// content as much as possible), returns whether it changed
static bool term_resize(int cols, int rows) {
  if (cols == term.cols && rows == term.rows)
    return false;
  struct cell *ng = static_cast<struct cell *>(
      calloc((size_t)cols * rows, sizeof(struct cell)));
  struct cell *na = static_cast<struct cell *>(
      calloc((size_t)cols * rows, sizeof(struct cell)));
  struct cell b = blank_cell();
  for (int i = 0; i < cols * rows; i++)
    ng[i] = na[i] = b;
  int mc = cols < term.cols ? cols : term.cols;
  int mr = rows < term.rows ? rows : term.rows;
  for (int y = 0; y < mr; y++)
    memcpy(&ng[y * cols], &term.grid[y * term.cols],
           (size_t)mc * sizeof(struct cell));
  free(term.grid);
  free(term.alt);
  term.grid = ng;
  term.alt = na;
  term.cols = cols;
  term.rows = rows;
  term.cx = CLAMP(term.cx, 0, cols - 1);
  term.cy = CLAMP(term.cy, 0, rows - 1);
  term.scroll_top = 0;
  term.scroll_bottom = rows - 1;
  term.wrap_pending = false;
  return true;
}

#ifdef TERMINAL_SGR_TEST
static char reply_buf[128];
static size_t reply_len;

static void term_reply(const char *s, size_t n) {
  size_t room = sizeof(reply_buf) - reply_len;
  if (n > room)
    n = room;
  memcpy(reply_buf + reply_len, s, n);
  reply_len += n;
}

void setUp(void) {}
void tearDown(void) {}

static void feed_test(const char *text) {
  term_feed((const uint8_t *)text, strlen(text));
}

static void test_sgr_attributes_and_resets(void) {
  feed_test("\033[4;7;9mX\033[24;27;29mY");
  TEST_ASSERT_EQUAL_UINT8(CELL_UNDERLINE | CELL_INVERSE | CELL_STRIKE,
                          cell_at(0, 0)->flags);
  TEST_ASSERT_EQUAL_UINT8(0, cell_at(1, 0)->flags);
}

static void test_bold_color_is_order_independent(void) {
  feed_test("\r\033[0;31;1mA\033[22mB\033[1;31mC");
  struct cell *a = cell_at(0, 0);
  struct cell *b = cell_at(1, 0);
  struct cell *c = cell_at(2, 0);
  TEST_ASSERT_EQUAL_UINT32(COLOR_INDEX(1), a->fg);
  TEST_ASSERT_EQUAL_UINT32(COLOR_INDEX(1), b->fg);
  TEST_ASSERT_EQUAL_UINT32(COLOR_INDEX(1), c->fg);
  TEST_ASSERT_BITS_HIGH(CELL_BOLD, a->flags);
  TEST_ASSERT_BITS_LOW(CELL_BOLD, b->flags);
  TEST_ASSERT_BITS_HIGH(CELL_BOLD, c->flags);
  TEST_ASSERT_EQUAL_HEX32(palette[9], resolve_color(a->fg, true));
  TEST_ASSERT_EQUAL_HEX32(palette[1], resolve_color(b->fg, false));
  TEST_ASSERT_EQUAL_HEX32(palette[9], resolve_color(c->fg, true));
}

static void test_extended_colors_and_reset(void) {
  feed_test("\r\033[0;38;5;196;48;2;1;2;3mZ\033[0mR");
  struct cell *colored = cell_at(0, 0);
  struct cell *reset = cell_at(1, 0);
  TEST_ASSERT_EQUAL_UINT32(COLOR_INDEX(196), colored->fg);
  TEST_ASSERT_EQUAL_HEX32(0x010203, colored->bg);
  TEST_ASSERT_EQUAL_UINT32(COLOR_DEFAULT, reset->fg);
  TEST_ASSERT_EQUAL_UINT32(COLOR_DEFAULT, reset->bg);
  TEST_ASSERT_EQUAL_UINT8(0, reset->flags);
}

static void test_origin_mode_coordinates_and_bounds(void) {
  feed_test("\033[2;4r\033[?6h\033[2;3H");
  TEST_ASSERT_TRUE(term.origin_mode);
  TEST_ASSERT_EQUAL_INT(2, term.cy);
  TEST_ASSERT_EQUAL_INT(2, term.cx);
  feed_test("\033[99B");
  TEST_ASSERT_EQUAL_INT(3, term.cy);
  feed_test("\033[?6l");
  TEST_ASSERT_FALSE(term.origin_mode);
  TEST_ASSERT_EQUAL_INT(0, term.cy);
  TEST_ASSERT_EQUAL_INT(0, term.cx);
}

static void test_dsr_da_and_cursor_shape(void) {
  reply_len = 0;
  feed_test("\033[3;4H\033[6n\033[c\033[>c");
  TEST_ASSERT_EQUAL_size_t(22, reply_len);
  TEST_ASSERT_EQUAL_MEMORY("\033[3;4R\033[?1;2c\033[>0;1;0c", reply_buf,
                           reply_len);
  feed_test("\033[3 q");
  TEST_ASSERT_EQUAL_INT(CURSOR_UNDERLINE, term.cursor_shape);
  feed_test("\033[6 q");
  TEST_ASSERT_EQUAL_INT(CURSOR_BAR, term.cursor_shape);
  feed_test("\033[0 q");
  TEST_ASSERT_EQUAL_INT(CURSOR_BLOCK, term.cursor_shape);
  feed_test("\033[?1h");
  TEST_ASSERT_TRUE(term.application_cursor);
  feed_test("\033[?1l");
  TEST_ASSERT_FALSE(term.application_cursor);
}

extern "C" int main(void) {
  palette_init();
  term_init(16, 6);
  UNITY_BEGIN();
  RUN_TEST(test_sgr_attributes_and_resets);
  RUN_TEST(test_bold_color_is_order_independent);
  RUN_TEST(test_extended_colors_and_reset);
  RUN_TEST(test_origin_mode_coordinates_and_bounds);
  RUN_TEST(test_dsr_da_and_cursor_shape);
  return UNITY_END();
}
#else

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
struct app {
  // wayland global objects
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct xdg_wm_base *wm_base;
  struct zxdg_decoration_manager_v1 *decoration_manager;
  struct wl_seat *seat;
  struct wl_output *output;
  struct wl_keyboard *keyboard;
  struct wl_pointer *pointer;
  int output_width, output_height, output_scale;

  // pointer state (surface-local coordinates)
  double ptr_x, ptr_y;
  bool
      ptr_hover_buttons; // hovering over the traffic lights (shows x/−/+ icons)

  // window
  struct wl_surface *surface;
  struct xdg_surface *xsurface;
  struct xdg_toplevel *toplevel;
  struct zxdg_toplevel_decoration_v1 *decoration;
  int width, height;
  bool closed;
  bool configured; // a buffer may only be committed after the first configure
  char title[256]; // title bar text (OSC 0/2 may change it)

  TerminalVulkanRenderer *renderer;
  int buffer_scale;

  // font metrics
  int cell_w, cell_h;
  double ascent;

  // PTY / shell
  int pty_fd;
  pid_t shell_pid;
  bool needs_render;

  // xkb keyboard state
  struct xkb_context *xkb_ctx;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
};

static struct app app = {
    .width = 720,
    .height = 460,
    .title = "vulkan-term",
    .buffer_scale = 1,
    .pty_fd = -1,
};

static void spawn_shell(void);
static void render(void);

static void frame_ready(void *data) {
  (void)data;
  if (!app.closed && app.shell_pid <= 0)
    spawn_shell();
  if (!app.closed && app.needs_render)
    render();
}

// Compute the grid column/row count from the window size
static void grid_dims(int *cols, int *rows) {
  *cols = (int)((app.width - 2 * TERM_PAD) / app.cell_w);
  *rows = (int)((app.height - 2 * TERM_PAD) / app.cell_h);
  *cols = CLAMP(*cols, 10, MAX_COLS);
  *rows = CLAMP(*rows, 3, MAX_ROWS);
}

// ---------------------------------------------------------------------------
// freetype font metrics + glyph atlas
// ---------------------------------------------------------------------------
// Glyph atlas: rasterize every needed character once into a single large
// texture; at draw time look up coordinates by codepoint and draw characters
// as textured quads. The terminal charset is stable, so one build is enough.
#define ATLAS_W 1024
#define ATLAS_H 1024
#define GLYPH_CACHE_SIZE 4096
#define ATLAS_PAD 1

struct glyph {
  uint32_t cp;              // codepoint
  int atlas_x, atlas_y;     // pixel position in the atlas
  int w, h;                 // glyph bitmap size
  int bearing_x, bearing_y; // offset from stroke origin to glyph top-left
  int advance;              // horizontal advance for this glyph (pixels)
};

static FT_Library ft_lib;
static FT_Face ft_face;
static double font_pixel_size = FONT_SIZE;
static uint8_t *atlas_pixels;
static uint64_t atlas_generation;
// Open-addressing hash table, so Unicode codepoints aren't limited to a small
// direct-index array.
static struct glyph atlas_glyphs[GLYPH_CACHE_SIZE];
static int atlas_count;
static int atlas_cursor_x = ATLAS_PAD;
static int atlas_cursor_y = ATLAS_PAD;
static int atlas_row_h;

static struct glyph *glyph_lookup(uint32_t cp) {
  if (cp == 0)
    return NULL;
  size_t slot = (cp * 2654435761u) & (GLYPH_CACHE_SIZE - 1);
  for (size_t i = 0; i < GLYPH_CACHE_SIZE; i++) {
    struct glyph *g = &atlas_glyphs[slot];
    if (g->cp == cp)
      return g;
    if (g->cp == 0)
      return NULL;
    slot = (slot + 1) & (GLYPH_CACHE_SIZE - 1);
  }
  return NULL;
}

static struct glyph *glyph_insert(struct glyph glyph) {
  size_t slot = (glyph.cp * 2654435761u) & (GLYPH_CACHE_SIZE - 1);
  for (size_t i = 0; i < GLYPH_CACHE_SIZE; i++) {
    struct glyph *g = &atlas_glyphs[slot];
    if (g->cp == 0 || g->cp == glyph.cp) {
      if (g->cp == 0)
        atlas_count++;
      *g = glyph;
      return g;
    }
    slot = (slot + 1) & (GLYPH_CACHE_SIZE - 1);
  }
  return NULL;
}

// Render one glyph into the atlas, recording its position and metrics
static struct glyph *atlas_add(FT_Face face, uint32_t cp) {
  struct glyph *cached = glyph_lookup(cp);
  if (cached)
    return cached;
  if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) ||
      !face->glyph->bitmap.buffer)
    return NULL;
  struct glyph g = {
      .cp = cp,
      .w = (int)face->glyph->bitmap.width,
      .h = (int)face->glyph->bitmap.rows,
      .bearing_x = face->glyph->bitmap_left,
      .bearing_y = face->glyph->bitmap_top,
      .advance = (int)(face->glyph->advance.x >> 6),
  };
  if (atlas_cursor_x + g.w + ATLAS_PAD > ATLAS_W) {
    atlas_cursor_x = ATLAS_PAD;
    atlas_cursor_y += atlas_row_h + ATLAS_PAD;
    atlas_row_h = 0;
  }
  if (atlas_cursor_y + g.h + ATLAS_PAD > ATLAS_H)
    return NULL;
  if (g.h > atlas_row_h)
    atlas_row_h = g.h;
  g.atlas_x = atlas_cursor_x;
  g.atlas_y = atlas_cursor_y;
  atlas_cursor_x += g.w + ATLAS_PAD;

  if (!atlas_pixels || g.w <= 0 || g.h <= 0)
    return NULL;
  FT_Bitmap *bitmap = &face->glyph->bitmap;
  size_t pitch =
      bitmap->pitch >= 0 ? (size_t)bitmap->pitch : (size_t)-bitmap->pitch;
  if (pitch == 0 || pitch > SIZE_MAX / (size_t)g.h)
    return NULL;
  for (int y = 0; y < g.h; y++) {
    const uint8_t *src = bitmap->pitch >= 0
                             ? bitmap->buffer + (size_t)y * bitmap->pitch
                             : bitmap->buffer + (size_t)(g.h - 1 - y) * pitch;
    for (int x = 0; x < g.w; x++) {
      uint8_t coverage;
      if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
        coverage = (src[x / 8] & (0x80 >> (x % 8))) ? 255 : 0;
      else if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY)
        coverage = src[x];
      else
        return NULL;
      atlas_pixels[(size_t)(g.atlas_y + y) * ATLAS_W + g.atlas_x + x] =
          coverage;
    }
  }
  ++atlas_generation;

  return glyph_insert(g);
}

static const char *find_font(void) {
  const char *font = getenv("TERMINAL_FONT");
  if (font != NULL && access(font, R_OK) == 0)
    return font;
  return "/usr/share/fonts/NotoMono-Regular.ttf";
}

static void measure_font(void) {
  if (FT_Init_FreeType(&ft_lib)) {
    fprintf(stderr, "freetype initialization failed\n");
    exit(1);
  }
  const char *font = find_font();
  if (!font) {
    fprintf(stderr, "no monospace font found (apt-get install fonts-dejavu)\n");
    exit(1);
  }
  if (FT_New_Face(ft_lib, font, 0, &ft_face)) {
    fprintf(stderr, "failed to load font %s\n", font);
    exit(1);
  }
  if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)lround(font_pixel_size))) {
    fprintf(stderr, "failed to set font size %.1fpx\n", font_pixel_size);
    exit(1);
  }

  // Measure cell width/height with 'M' (all chars in a monospace font share
  // the same advance)
  FT_Load_Char(ft_face, 'M', FT_LOAD_TARGET_LIGHT);
  app.cell_w = (int)(ft_face->glyph->advance.x >> 6);
  FT_Load_Char(ft_face, 'M', FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT);
  int ascent = ft_face->size->metrics.ascender >> 6;
  int descent = ft_face->size->metrics.descender >> 6;
  app.cell_h = ascent - descent;
  app.ascent = ascent;

  // Font metrics are independent of Vulkan resource creation.
}

static void atlas_init(void) {
  atlas_pixels = static_cast<uint8_t *>(calloc((size_t)ATLAS_W, ATLAS_H));
  if (!atlas_pixels) {
    fprintf(stderr, "failed to allocate glyph atlas\n");
    exit(1);
  }
  for (uint32_t c = 0x20; c < 0x7f; c++)
    atlas_add(ft_face, c);
  // Common non-ASCII: box-drawing/shade characters (used by htop, dialog
  // borders)
  static const uint32_t extra[] = {
      0x2500, 0x2501, 0x2502, 0x2503, 0x250c, 0x250f, 0x2510,
      0x2513, 0x2514, 0x2517, 0x2518, 0x251b, 0x251c, 0x251f,
      0x2523, 0x252b, 0x2533, 0x253b, 0x254b, 0x2580, 0x2584,
      0x2588, 0x258c, 0x2590, 0x2591, 0x2592, 0x2593, 0};
  for (int i = 0; extra[i]; i++)
    atlas_add(ft_face, extra[i]);
  fprintf(stderr, "[TERM-VK] atlas initialized glyphs=%d generation=%llu\n",
          atlas_count, (unsigned long long)atlas_generation);
}

// Look up the glyph for a codepoint; if not in the atlas, add it on demand
// (the terminal may receive arbitrary Unicode)
static struct glyph *get_glyph(uint32_t cp) {
  struct glyph *g = glyph_lookup(cp);
  return g ? g : atlas_add(ft_face, cp);
}

// Convert 0xRRGGBB + alpha into premultiplied RGBA normalized floats
static void color_premul(uint32_t rgb, float alpha, float out[4]) {
  float r = ((rgb >> 16) & 0xFF) / 255.0f;
  float g = ((rgb >> 8) & 0xFF) / 255.0f;
  float b = (rgb & 0xFF) / 255.0f;
  out[0] = r * alpha;
  out[1] = g * alpha;
  out[2] = b * alpha;
  out[3] = alpha;
}

static bool make_glyph_quad(uint32_t cp, float x, float baseline,
                            const float color[4], TerminalGlyphQuad *quad) {
  struct glyph *g = get_glyph(cp);
  if (!g) {
    static bool glyph_failure_logged;
    if (!glyph_failure_logged) {
      fprintf(stderr,
              "[TERM-VK] glyph lookup failed cp=U+%04X cache=%d "
              "atlas_generation=%llu\n",
              cp, atlas_count, (unsigned long long)atlas_generation);
      glyph_failure_logged = true;
    }
    return false;
  }
  *quad = {
      .x = x + g->bearing_x,
      .y = baseline - g->bearing_y,
      .width = (float)g->w,
      .height = (float)g->h,
      .u0 = (float)g->atlas_x / ATLAS_W,
      .v0 = (float)g->atlas_y / ATLAS_H,
      .u1 = (float)(g->atlas_x + g->w) / ATLAS_W,
      .v1 = (float)(g->atlas_y + g->h) / ATLAS_H,
  };
  memcpy(quad->color, color, sizeof(quad->color));
  return true;
}

static void render_now(void) {
  if (!app.configured || !app.renderer)
    return;
  app.needs_render = false;
  size_t cells = (size_t)term.cols * term.rows;
  TerminalRect *backgrounds =
      static_cast<TerminalRect *>(calloc(cells + 1, sizeof(TerminalRect)));
  TerminalGlyphQuad *glyphs = static_cast<TerminalGlyphQuad *>(
      calloc(cells * 2 + 1, sizeof(TerminalGlyphQuad)));
  TerminalRect *decorations =
      static_cast<TerminalRect *>(calloc(cells * 2 + 1, sizeof(TerminalRect)));
  TerminalRect cursor[1]{};
  TerminalGlyphQuad cursor_glyph[1]{};
  if (!backgrounds || !glyphs || !decorations) {
    free(backgrounds);
    free(glyphs);
    free(decorations);
    fprintf(stderr, "[TERM-VK] FATAL draw-list allocation failed\n");
    app.closed = true;
    return;
  }
  size_t background_count = 0, glyph_count = 0, decoration_count = 0;
  size_t cursor_count = 0, cursor_glyph_count = 0;
  float body[4];
  color_premul(0x17171a, 0.84f, body);
  backgrounds[background_count] = {0, 0, (float)app.width, (float)app.height};
  memcpy(backgrounds[background_count++].color, body, sizeof(body));

  struct cell *grid = cur_grid();
  for (int y = 0; y < term.rows; y++) {
    float row_y = TERM_PAD + y * app.cell_h;
    float baseline = row_y + app.ascent;
    for (int x = 0; x < term.cols; x++) {
      struct cell *c = &grid[y * term.cols + x];
      bool bold = c->flags & CELL_BOLD;
      uint32_t fg = resolve_color(c->fg, bold);
      uint32_t bg = resolve_color(c->bg, false);
      if (c->flags & CELL_INVERSE) {
        uint32_t t = fg;
        fg = (bg == COLOR_DEFAULT) ? 0x1a1a24 : bg;
        bg = (t == COLOR_DEFAULT) ? 0xd9e6d9 : t;
      }
      float px = TERM_PAD + x * app.cell_w;
      // Background color block (default color isn't drawn, exposing the window
      // body)
      if (bg != COLOR_DEFAULT) {
        float bgc[4];
        color_premul(bg, 1.0f, bgc);
        TerminalRect *rect = &backgrounds[background_count++];
        *rect = {px, row_y, (float)app.cell_w, (float)app.cell_h};
        memcpy(rect->color, bgc, sizeof(bgc));
      }
      float fgc[4];
      if (fg == COLOR_DEFAULT)
        color_premul(0xd9e6d9, 1.0f, fgc);
      else
        color_premul(fg, 1.0f, fgc);
      // Glyph
      if (c->cp) {
        if (make_glyph_quad(c->cp, px, baseline, fgc, &glyphs[glyph_count]))
          ++glyph_count;
        if (bold)
          if (make_glyph_quad(c->cp, px + 0.65f, baseline, fgc,
                              &glyphs[glyph_count]))
            ++glyph_count;
      }
      if (c->flags & CELL_UNDERLINE) {
        TerminalRect *rect = &decorations[decoration_count++];
        *rect = {px, baseline + 1.0f, (float)app.cell_w, 1.0f};
        memcpy(rect->color, fgc, sizeof(fgc));
      }
      if (c->flags & CELL_STRIKE) {
        TerminalRect *rect = &decorations[decoration_count++];
        *rect = {px, baseline - (float)app.ascent * 0.32f, (float)app.cell_w,
                 1.0f};
        memcpy(rect->color, fgc, sizeof(fgc));
      }
    }
  }

  // DECSCUSR cursor shape; blinking currently uses the matching static shape.
  if (term.cursor_visible) {
    float px = TERM_PAD + term.cx * app.cell_w;
    float py = TERM_PAD + term.cy * app.cell_h;
    float cur[4];
    color_premul(0xd9e6d9, 0.85f, cur);
    float cursor_x = px;
    float cursor_y = py;
    float cursor_w = app.cell_w;
    float cursor_h = app.cell_h;
    if (term.cursor_shape == CURSOR_UNDERLINE) {
      cursor_y = py + app.cell_h - 2.0f;
      cursor_h = 2.0f;
    } else if (term.cursor_shape == CURSOR_BAR) {
      cursor_w = 2.0f;
    }
    cursor[0] = {cursor_x, cursor_y, cursor_w, cursor_h};
    memcpy(cursor[0].color, cur, sizeof(cur));
    cursor_count = 1;
    struct cell *c = cell_at(term.cx, term.cy);
    if (c->cp && term.cursor_shape == CURSOR_BLOCK) {
      float inv[4];
      color_premul(0x17171a, 1.0f, inv);
      if (make_glyph_quad(c->cp, px, py + app.ascent, inv, cursor_glyph))
        cursor_glyph_count = 1;
    }
  }

  TerminalDrawList list = {
      .backgrounds = {backgrounds, background_count},
      .glyphs = {glyphs, glyph_count},
      .decorations = {decorations, decoration_count},
      .cursor = {cursor, cursor_count},
      .cursor_glyphs = {cursor_glyph, cursor_glyph_count},
  };
  TerminalFrame frame = {
      .draw_list = &list,
      .atlas_pixels = atlas_pixels,
      .atlas_width = ATLAS_W,
      .atlas_height = ATLAS_H,
      .atlas_generation = atlas_generation,
      .logical_width = (uint32_t)app.width,
      .logical_height = (uint32_t)app.height,
  };
  static bool content_attempt_logged;
  if (!content_attempt_logged && glyph_count) {
    fprintf(stderr,
            "[TERM-VK] content draw-list glyphs=%zu atlas_generation=%llu\n",
            glyph_count, (unsigned long long)atlas_generation);
    content_attempt_logged = true;
  }
  TerminalVkRenderResult result = terminal_vk_render(app.renderer, &frame);
  free(backgrounds);
  free(glyphs);
  free(decorations);
  if (result == TERMINAL_VK_FATAL) {
    app.closed = true;
  } else if (result == TERMINAL_VK_RETRY) {
    app.needs_render = true;
  } else {
    static bool perf_marked;
    if (!perf_marked) {
      (void)syscall(SYS_PERF, XOS_PERF_COUNTER_MARK,
                    XOS_PERF_GUI_TERMINAL_FIRST_BUFFER, 0, 0, 0, 0);
      perf_marked = true;
    }
  }
}

static void render(void) {
  app.needs_render = true;
  render_now();
}

// ---------------------------------------------------------------------------
// OSC completion: 0/1/2 set the window title
// ---------------------------------------------------------------------------
static void osc_done(void) {
  term.osc[term.osc_len] = '\0';
  char *semi = strchr(term.osc, ';');
  if (!semi)
    return;
  int code = atoi(term.osc);
  if (code != 0 && code != 1 && code != 2)
    return;
  snprintf(app.title, sizeof(app.title), "%s", semi + 1);
  if (app.toplevel)
    xdg_toplevel_set_title(app.toplevel, app.title);
  // Title changed, so redraw; the caller renders after the call site.
}

// ---------------------------------------------------------------------------
// PTY: spawn the shell
// ---------------------------------------------------------------------------
static void spawn_shell(void) {
  int cols, rows;
  grid_dims(&cols, &rows);
  struct winsize ws = {
      .ws_row = (unsigned short)rows,
      .ws_col = (unsigned short)cols,
  };
  pid_t pid = forkpty(&app.pty_fd, NULL, NULL, &ws);
  if (pid < 0) {
    perror("forkpty");
    exit(1);
  }
  if (pid == 0) {
    // Child process: the PTY slave is already stdin/stdout/stderr and the
    // controlling terminal
    setenv("TERM", "xterm-256color", 1);
    const char *shell = getenv("SHELL");
    if (!shell || !*shell)
      shell = "/bin/sh";
    execlp(shell, shell, (char *)NULL);
    _exit(127);
  }
  app.shell_pid = pid;
  fprintf(stderr, "[TERM-VK] shell spawned pid=%d pty_fd=%d\n", (int)pid,
          app.pty_fd);
  int flags = fcntl(app.pty_fd, F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(app.pty_fd, F_SETFL, flags | O_NONBLOCK);
  signal(SIGCHLD, SIG_IGN); // auto-reap, avoid zombie processes
  signal(SIGPIPE, SIG_IGN); // don't die on PTY write if the shell exited first
}

static void pty_write(const char *s, size_t n) {
  if (app.pty_fd >= 0)
    (void)write(app.pty_fd, s, n);
}

static void term_reply(const char *s, size_t n) { pty_write(s, n); }

// Notify the shell the window size changed (SIGWINCH is sent by the kernel)
static void pty_resize(void) {
  if (app.pty_fd < 0)
    return;
  struct winsize ws = {
      .ws_row = (unsigned short)term.rows,
      .ws_col = (unsigned short)term.cols,
  };
  ioctl(app.pty_fd, TIOCSWINSZ, &ws);
}

// ---------------------------------------------------------------------------
// xdg-shell callbacks
// ---------------------------------------------------------------------------
static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial) {
  xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xsurface_configure(void *data, struct xdg_surface *xsurface,
                               uint32_t serial) {
  xdg_surface_ack_configure(xsurface, serial);
  static bool perf_marked;
  if (!perf_marked && syscall(SYS_PERF, XOS_PERF_COUNTER_MARK,
                              XOS_PERF_GUI_TERMINAL_XDG_READY, 0, 0, 0, 0) == 0)
    perf_marked = true;
  app.configured = true;
  if (!app.renderer &&
      !terminal_vk_create(&app.renderer, app.display, app.surface,
                          (uint32_t)app.width, (uint32_t)app.height,
                          (uint32_t)app.buffer_scale, frame_ready, NULL)) {
    fprintf(stderr, "[TERM-VK] FATAL renderer initialization failed\n");
    app.closed = true;
    return;
  }
  terminal_vk_resize(app.renderer, (uint32_t)app.width, (uint32_t)app.height,
                     (uint32_t)app.buffer_scale);
  int cols, rows;
  grid_dims(&cols, &rows);
  if (term_resize(cols, rows))
    pty_resize();
  render();
}

static const struct xdg_surface_listener xsurface_listener = {
    .configure = xsurface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states) {
  if (width > 0 && height > 0) {
    app.width = width;
    app.height = height;
  }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
  app.closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

// ---------------------------------------------------------------------------
// Keyboard input: translate into a terminal byte stream written to the PTY
// ---------------------------------------------------------------------------
static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                            int32_t fd, uint32_t size) {
  char *map =
      static_cast<char *>(mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0));
  if (map == MAP_FAILED) {
    close(fd);
    return;
  }
  app.xkb_keymap = xkb_keymap_new_from_string(
      app.xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map, size);
  close(fd);
  app.xkb_state = xkb_state_new(app.xkb_keymap);
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state) {
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !app.xkb_state)
    return;

  xkb_keycode_t kc = key + 8;
  xkb_keysym_t sym = xkb_state_key_get_one_sym(app.xkb_state, kc);
  bool ctrl =
      xkb_state_mod_name_is_active(
          app.xkb_state, XKB_MOD_NAME_CTRL,
          static_cast<enum xkb_state_component>(XKB_STATE_EFFECTIVE)) > 0;
  bool shift =
      xkb_state_mod_name_is_active(
          app.xkb_state, XKB_MOD_NAME_SHIFT,
          static_cast<enum xkb_state_component>(XKB_STATE_EFFECTIVE)) > 0;

  switch (sym) {
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    pty_write("\r", 1);
    return;
  case XKB_KEY_BackSpace:
    pty_write("\x7f", 1);
    return;
  case XKB_KEY_Escape:
    pty_write("\x1b", 1);
    return;
  case XKB_KEY_Tab:
  case XKB_KEY_KP_Tab:
    pty_write(shift ? "\x1b[Z" : "\t", shift ? 3 : 1);
    return;
  case XKB_KEY_Up:
    pty_write(term.application_cursor ? "\x1bOA" : "\x1b[A", 3);
    return;
  case XKB_KEY_Down:
    pty_write(term.application_cursor ? "\x1bOB" : "\x1b[B", 3);
    return;
  case XKB_KEY_Right:
    pty_write(term.application_cursor ? "\x1bOC" : "\x1b[C", 3);
    return;
  case XKB_KEY_Left:
    pty_write(term.application_cursor ? "\x1bOD" : "\x1b[D", 3);
    return;
  case XKB_KEY_Home:
    pty_write(term.application_cursor ? "\x1bOH" : "\x1b[H", 3);
    return;
  case XKB_KEY_End:
    pty_write(term.application_cursor ? "\x1bOF" : "\x1b[F", 3);
    return;
  case XKB_KEY_Prior:
    pty_write("\x1b[5~", 4);
    return; // PgUp
  case XKB_KEY_Next:
    pty_write("\x1b[6~", 4);
    return; // PgDn
  case XKB_KEY_Delete:
    pty_write("\x1b[3~", 4);
    return;
  case XKB_KEY_Insert:
    pty_write("\x1b[2~", 4);
    return;
  }

  // Ctrl+letter → control character (Ctrl+C=0x03, Ctrl+D=0x04, Ctrl+L=0x0C,
  // ...)
  if (ctrl) {
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
      char c = (char)(sym - XKB_KEY_a + 1);
      pty_write(&c, 1);
      return;
    }
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) { // Ctrl+Shift+letter
      char c = (char)(sym - XKB_KEY_A + 1);
      pty_write(&c, 1);
      return;
    }
    if (sym == XKB_KEY_space) {
      pty_write("\x00", 1);
      return;
    }
    if (sym == XKB_KEY_bracketleft) {
      pty_write("\x1b", 1);
      return;
    }
    if (sym == XKB_KEY_backslash) {
      pty_write("\x1c", 1);
      return;
    }
    if (sym == XKB_KEY_bracketright) {
      pty_write("\x1d", 1);
      return;
    }
  }

  char buf[8];
  int n = xkb_state_key_get_utf8(app.xkb_state, kc, buf, sizeof(buf));
  if (n > 0)
    pty_write(buf, n);
  // No local echo: only output echoed back by the shell through the PTY
  // triggers render().
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group) {
  if (app.xkb_state)
    xkb_state_update_mask(app.xkb_state, depressed, latched, locked, 0, 0,
                          group);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface) {}
static void keyboard_repeat_info(void *data, struct wl_keyboard *kb,
                                 int32_t rate, int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

extern const struct wl_pointer_listener pointer_listener;

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app.keyboard) {
    app.keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(app.keyboard, &keyboard_listener, NULL);
  }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}

// ---------------------------------------------------------------------------
// Pointer input: drag window via title bar, close via red button, traffic-light
// hover
// ---------------------------------------------------------------------------
// Hit test: traffic light centers at (20/40/60, TITLEBAR_H/2), generous click
// radius
static int hit_traffic_button(double x, double y) {
  static const double btn_x[3] = {20, 40, 60};
  for (int i = 0; i < 3; i++) {
    double dx = x - btn_x[i], dy = y - TITLEBAR_H / 2;
    if (dx * dx + dy * dy <= 9 * 9)
      return i; // 0=red 1=yellow 2=green
  }
  return -1;
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy) {
  app.ptr_x = wl_fixed_to_double(sx);
  app.ptr_y = wl_fixed_to_double(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
  if (app.ptr_hover_buttons) {
    app.ptr_hover_buttons = false;
    render();
  }
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
  app.ptr_x = wl_fixed_to_double(sx);
  app.ptr_y = wl_fixed_to_double(sy);
  // hover region: the traffic lights at the left of the title bar
  bool hover = app.ptr_y < TITLEBAR_H && app.ptr_x < 80;
  if (hover != app.ptr_hover_buttons) {
    app.ptr_hover_buttons = hover;
    render();
  }
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
  if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
    return;
  if (app.ptr_y >= TITLEBAR_H)
    return; // click in the body area: no action

  int btn = hit_traffic_button(app.ptr_x, app.ptr_y);
  if (btn == 0) {
    app.closed = true; // red button: close the window
  } else if (btn < 0) {
    // empty title bar: ask the compositor to start interactive drag-move
    xdg_toplevel_move(app.toplevel, app.seat, serial);
  }
  // yellow/green buttons: purely decorative, no action
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {}

const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
                            int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {
  (void)data;
  (void)output;
  (void)x;
  (void)y;
  (void)physical_width;
  (void)physical_height;
  (void)subpixel;
  (void)make;
  (void)model;
  (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
  (void)data;
  (void)output;
  (void)refresh;
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    app.output_width = width;
    app.output_height = height;
  }
}

static void output_done(void *data, struct wl_output *output) {
  (void)data;
  (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t scale) {
  (void)data;
  (void)output;
  app.output_scale = scale > 0 ? scale : 1;
  if (app.surface && app.buffer_scale != app.output_scale) {
    app.buffer_scale = app.output_scale;
    wl_surface_set_buffer_scale(app.surface, app.buffer_scale);
    if (app.renderer)
      terminal_vk_resize(app.renderer, (uint32_t)app.width,
                         (uint32_t)app.height, (uint32_t)app.buffer_scale);
    render();
  }
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------
static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    app.compositor = static_cast<struct wl_compositor *>(
        wl_registry_bind(registry, name, &wl_compositor_interface, 4));
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    app.wm_base = static_cast<struct xdg_wm_base *>(
        wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    xdg_wm_base_add_listener(app.wm_base, &wm_base_listener, NULL);
  } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) ==
             0) {
    app.decoration_manager =
        static_cast<struct zxdg_decoration_manager_v1 *>(wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1));
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    app.seat = static_cast<struct wl_seat *>(
        wl_registry_bind(registry, name, &wl_seat_interface, 1));
    wl_seat_add_listener(app.seat, &seat_listener, NULL);
  } else if (strcmp(interface, wl_output_interface.name) == 0 && !app.output) {
    uint32_t bind_version = version < 2 ? version : 2;
    app.output = static_cast<struct wl_output *>(
        wl_registry_bind(registry, name, &wl_output_interface, bind_version));
    wl_output_add_listener(app.output, &output_listener, NULL);
  }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// ---------------------------------------------------------------------------
extern "C" int main(void) {
  palette_init();

  app.display = wl_display_connect(NULL);
  if (!app.display) {
    fprintf(stderr,
            "failed to connect to Wayland display (run inside tinywl)\n");
    return 1;
  }
  app.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  struct wl_registry *registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(app.display);
  if (app.output_width > 0 && app.output_height > 0) {
    int scale = app.output_scale > 0 ? app.output_scale : 1;
    int screen_width = app.output_width / scale;
    int screen_height = app.output_height / scale;
    app.width = CLAMP(screen_width * 70 / 100, 400, screen_width - 40);
    app.height = CLAMP(screen_height * 65 / 100, 300, screen_height - 80);
    font_pixel_size = CLAMP(screen_height / 43.0, 12.0, 16.0);
  }
  measure_font();
  atlas_init();
  int cols, rows;
  grid_dims(&cols, &rows);
  term_init(cols, rows);

  if (!app.compositor || !app.wm_base) {
    fprintf(stderr, "compositor missing required protocols\n");
    return 1;
  }

  app.surface = wl_compositor_create_surface(app.compositor);
  app.buffer_scale = app.output_scale > 0 ? app.output_scale : 1;
  wl_surface_set_buffer_scale(app.surface, app.buffer_scale);
  app.xsurface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
  xdg_surface_add_listener(app.xsurface, &xsurface_listener, NULL);
  app.toplevel = xdg_surface_get_toplevel(app.xsurface);
  xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, NULL);
  if (app.decoration_manager != NULL) {
    app.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
        app.decoration_manager, app.toplevel);
    zxdg_toplevel_decoration_v1_set_mode(
        app.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }
  xdg_toplevel_set_title(app.toplevel, app.title);
  xdg_toplevel_set_app_id(app.toplevel, "vulkan-term");
  wl_surface_commit(app.surface); // empty commit triggers the first configure

  // Event loop: wait for both Wayland events and PTY output
  int wl_fd = wl_display_get_fd(app.display);
  while (!app.closed) {
    while (wl_display_prepare_read(app.display) != 0)
      wl_display_dispatch_pending(app.display);
    if (wl_display_flush(app.display) < 0 && errno != EAGAIN) {
      wl_display_cancel_read(app.display);
      break;
    }

    struct pollfd fds[2] = {
        {.fd = wl_fd, .events = POLLIN},
        {.fd = app.pty_fd, .events = POLLIN},
    };
    if (poll(fds, 2, -1) < 0) {
      wl_display_cancel_read(app.display);
      if (errno == EINTR)
        continue;
      break;
    }

    if (fds[0].revents & POLLIN)
      wl_display_read_events(app.display);
    else
      wl_display_cancel_read(app.display);
    wl_display_dispatch_pending(app.display);

    if (app.closed)
      break;

    if (fds[1].revents & (POLLIN | POLLHUP)) {
      uint8_t buf[4096];
      bool got_output = false;
      // Bound each drain pass so a chatty process cannot starve keyboard and
      // close events on the Wayland fd.
      for (int chunks = 0; chunks < 16; chunks++) {
        ssize_t n = read(app.pty_fd, buf, sizeof(buf));
        if (n > 0) {
          static bool pty_logged;
          bool log_this_chunk = !pty_logged;
          if (log_this_chunk) {
            fprintf(stderr, "[TERM-VK] first PTY output bytes=%zd\n", n);
            pty_logged = true;
          }
          term_feed(buf, (size_t)n);
          if (log_this_chunk) {
            size_t occupied = 0;
            for (int cell = 0; cell < term.cols * term.rows; ++cell)
              if (cur_grid()[cell].cp)
                ++occupied;
            fprintf(stderr,
                    "[TERM-VK] parser occupied_cells=%zu cursor=%d,%d\n",
                    occupied, term.cx, term.cy);
          }
          got_output = true;
          continue;
        }
        if (n < 0 && errno == EINTR)
          continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
          break;
        // Shell exited (exit / Ctrl+D) or PTY hit an unrecoverable error: close
        // the window
        app.closed = true;
        break;
      }
      if (got_output)
        render();
    }
  }

  // Close the PTY master; the shell receives SIGHUP and follows
  if (app.pty_fd >= 0)
    close(app.pty_fd);
  if (app.shell_pid > 0)
    kill(app.shell_pid, SIGHUP);
  terminal_vk_destroy(&app.renderer);
  free(atlas_pixels);
  if (ft_face)
    FT_Done_Face(ft_face);
  if (ft_lib)
    FT_Done_FreeType(ft_lib);
  wl_display_disconnect(app.display);
  return 0;
}
#endif
