/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Terminal process (libinput variant): VT100 state machine + cell buffer +
// compositor. Uses libinput for keyboard event processing via
// libinput_udev_create_context() + libinput_udev_assign_seat() — 设备增删由
// udev 后端 monitor 自动管理,终端经 udevd 管道收热插拔 uevent。
// The kernel ringbuf_fops provides the read() backend from evdev's SHM ring.
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
#include <libudev.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int master_fd = -1;
static pid_t shell_pid = -1;

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
};

static struct cell *cells;
static int dirty_row_start;
static int dirty_row_end;

static struct vt100_state vt;

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
  switch (final_ch) {
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
    for (int c = vt.cursor_x; c < vt.cols; c++) {
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
      for (int i = 0; i < 8; i++)
        vt.csi_params[i] = 0;
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
    } else if (c == ';')
      vt.csi_param_count++;
    else if (c >= 0x40 && c <= 0x7E) {
      vt.csi_param_count++;
      vt100_csi_dispatch(c);
      vt.escape_state = VT100_NORMAL;
    }
    break;
  }
}

static void flush_dirty_cells() {
  if (dirty_row_start >= dirty_row_end)
    return;
  for (int row = dirty_row_start; row < dirty_row_end; row++)
    for (int col = 0; col < vt.cols; col++) {
      struct cell *c = &cells[row * vt.cols + col];
      display_client_render_cell(row, col, c->ch, c->fg_color, c->bg_color);
    }
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
  (void)user_data;
  int fd = open(path, flags);
  return fd >= 0 ? fd : -errno;
}

static void close_restricted(int fd, void *user_data) {
  (void)user_data;
  close(fd);
}

static const struct libinput_interface interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

// ===================== monitor 重连(M4) =====================

// spin 判定 monitor 死后:suspend 清死 monitor + 6×sleep(1) 退避 resume
// 重建(§3.3)。 resume 失败可重试:enable_receiving 失败路径干净清理保
// udev_monitor==NULL(:259 短路守卫不触发)。
static int resume_reconnect(struct libinput *li) {
  libinput_suspend(li); /* udev_input_disable:unref+置 NULL+删 epoll source */
  for (int attempt = 0; attempt < 6;
       attempt++) { /* 覆盖 init START_LIMIT_BURST=5 的 ~5s 窗口 + 1 余量 */
    write(2, "R", 1); /* 单字符进度:rebuild 尝试 */
    if (libinput_resume(li) == 0)
      return 0; /* resume 成功:monitor 重建 + udev_input_add_devices 重开设备 */
    sleep(1); /* udevd 重启窗口 socket 名空洞,退避(对齐 init RESTART_SEC=1) */
  }
  return -1; /* 6 次耗尽 → enter_degraded_hold */
}

// 6 次耗尽(§3.4):停紧邻重试,保 pty/shell 活(会话不丢),输入断,5s 低频探活自愈。
// 填补本 OS 无 systemd 保活 udevd 与 Linux"udevd 回来自然恢复"语义差。
// 探活期间持续 drain master_fd(pty 输出)并渲染——否则 pty 缓冲撑满会反压
// shell 写阻塞,与"shell 活/会话不丢"相悖(§3.4 设计意图)。master_fd 为
// O_NONBLOCK(read 返 -1/EAGAIN 即无数据),故 drain 不阻塞探活节奏。
static void enter_degraded_hold(struct libinput *li) {
  write(2, "D", 1); /* 单字符进度:degraded */
  fprintf(stderr, "terminal: monitor dead, input degraded\n");
  while (1) {
    sleep(5); /* 低频探活 */
    if (libinput_resume(li) == 0)
      return; /* 探活成功:monitor 重建,返回主循环恢复输入 */
    /* drain pty 输出保 shell 活(会话不丢) */
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

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;

  if (display_client_init() < 0) {
    printf("terminal: display_client_init FAILED\n");
    while (1) {
      struct recv_msg m;
      recv(&m, NULL, 0, 0);
    }
  }

  // Create libinput context (udev backend):udev_new + udev_create_context +
  // assign_seat("seat0")。assign_seat → udev_input_enable 建 udevd monitor +
  // 首轮 enumerate 直读 /sys 补现有设备(§2.1/§2.4)。
  // udevd 未起/crash → assign_seat 失败 → 黑屏(有意信号,不回退 path
  // 后端,§2.4)。
  struct udev *udev = udev_new();
  struct libinput *li = NULL;
  while (!li) {
    li = libinput_udev_create_context(&interface, NULL, udev);
    if (!li) {
      struct recv_msg m;
      recv(&m, NULL, 0, 1);
    }
  }

  // Set up libinput logging (after ctx created)
  libinput_log_set_handler(li, libinput_log);
  libinput_log_set_priority(li, LIBINPUT_LOG_PRIORITY_DEBUG);

  // assign_seat 触发首轮 enumerate + 建 monitor(取代显式 path_add_device)。
  // 设备增删事件由 udev 后端自动管理,终端无需处理 ADDED/REMOVED(§2.3)。
  if (libinput_udev_assign_seat(li, "seat0") != 0) {
    printf("terminal: libinput_udev_assign_seat FAILED (udevd down?)\n");
    while (1) {
      struct recv_msg m;
      recv(&m, NULL, 0, 0);
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

  int cell_bytes = vt.rows * vt.cols * sizeof(struct cell);
  cells =
      (struct cell *)mmap(NULL, cell_bytes, PROT_READ | PROT_WRITE, 0, -1, 0);
  if (!cells) {
    printf("terminal: mmap cells FAILED\n");
    while (1) {
      struct recv_msg m;
      recv(&m, NULL, 0, 0);
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

  master_fd = open("/dev/ptmx", O_RDWR);
  if (master_fd < 0) {
    printf("terminal: failed to open /dev/ptmx\n");
    return 1;
  }

  int pty_idx;
  ioctl(master_fd, TIOCGPTN, &pty_idx);
  char pts_path[16];
  snprintf(pts_path, sizeof(pts_path), "/dev/pts%d", pty_idx);

  shell_pid = fork();
  if (shell_pid == 0) {
    int slave_fd = open(pts_path, O_RDWR);
    if (slave_fd < 0) {
      write(2, "shell: failed to open slave\n", 29);
      _exit(127);
    }
    dup2(slave_fd, 0);
    dup2(slave_fd, 1);
    dup2(slave_fd, 2);
    if (slave_fd > 2)
      close(slave_fd);
    close(master_fd);
    execve("/usr/bin/shell", NULL, NULL);
    write(2, "shell_child: execve FAILED\n", 28);
    _exit(127);
  }

  fcntl(master_fd, F_SETFL, O_RDWR | O_NONBLOCK);

  struct winsize ws;
  ws.ws_row = display_rows;
  ws.ws_col = display_cols;
  ws.ws_xpixel = 0;
  ws.ws_ypixel = 0;
  ioctl(master_fd, TIOCSWINSZ, &ws);

  char linebuf[256];
  int linebuf_len = 0;

  // monitor 死亡检测:连续「零真实事件即时返回」计数(§3.2)。EOF busy-spin
  // 是 level-triggered epoll 下 pipe rd 常驻可读的唯一可观测副作用(§3.1)。
  int spin_count = 0;
  const int SPIN_THRESHOLD = 2000;

  while (1) {
    struct termios t;
    ioctl(master_fd, TCGETS, &t);

    // Drain all pending keyboard events via libinput (non-blocking dispatch)
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
        if (ascii_len <= 0) {
          libinput_event_destroy(lev);
          continue;
        }

        pid_t fg_pgid = shell_pid;
        int tmp_pgid;
        if (ioctl(master_fd, TIOCGPGRP, &tmp_pgid) == 0 && tmp_pgid > 0)
          fg_pgid = tmp_pgid;

        if ((t.c_lflag & ISIG) && ascii_buf[0] == (char)t.c_cc[VINTR]) {
          kill(-fg_pgid, SIGINT);
          libinput_event_destroy(lev);
          continue;
        }
        if ((t.c_lflag & ISIG) && ascii_buf[0] == (char)t.c_cc[VSUSP]) {
          kill(-fg_pgid, SIGTSTP);
          libinput_event_destroy(lev);
          continue;
        }

        if (t.c_lflag & ICANON) {
          if (ascii_buf[0] == '\n') {
            linebuf[linebuf_len++] = '\n';
            write(master_fd, linebuf, linebuf_len);
            vt100_feed('\r');
            vt100_feed('\n');
            linebuf_len = 0;
            libinput_event_destroy(lev);
            continue;
          }
          if (ascii_buf[0] == (char)t.c_cc[VEOF]) {
            if (linebuf_len > 0) {
              write(master_fd, linebuf, linebuf_len);
              linebuf_len = 0;
            } else
              write(master_fd, "", 0);
            libinput_event_destroy(lev);
            continue;
          }
          if (ascii_buf[0] == (char)t.c_cc[VERASE]) {
            if (linebuf_len > 0) {
              linebuf_len--;
              vt100_feed('\b');
              vt100_feed(' ');
              vt100_feed('\b');
            }
            libinput_event_destroy(lev);
            continue;
          }
          if (ascii_buf[0] == (char)t.c_cc[VKILL]) {
            linebuf_len = 0;
            vt100_feed('\r');
            vt100_feed('\n');
            libinput_event_destroy(lev);
            continue;
          }
          if (linebuf_len < (int)sizeof(linebuf) - 2) {
            if (ascii_len > 1)
              write(master_fd, ascii_buf, (size_t)ascii_len);
            else {
              linebuf[linebuf_len++] = ascii_buf[0];
              if (t.c_lflag & ECHO)
                vt100_feed(ascii_buf[0]);
            }
          }
          libinput_event_destroy(lev);
          continue;
        }

        write(master_fd, ascii_buf, (size_t)ascii_len);
        if (t.c_lflag & ECHO) {
          for (int i = 0; i < ascii_len; i++)
            vt100_feed(ascii_buf[i]);
        }
      } /* if KEYBOARD_KEY */
      libinput_event_destroy(lev);
    }

    // Shell output
    char buf[4096];
    int64_t n = read(master_fd, buf, sizeof(buf));
    if (n > 0) {
      got_event = 1; /* pty 数据算真实活动,复位 spin */
      for (int64_t i = 0; i < n; i++) {
        vt100_feed(buf[i]);
        if (dirty_row_end - dirty_row_start >= 4)
          flush_dirty_cells();
      }
    } else if (n == 0) {
      printf("terminal: shell exited (n=0), re-forking\n");
      close(master_fd);
      master_fd = open("/dev/ptmx", O_RDWR);
      if (master_fd < 0)
        continue;
      fcntl(master_fd, F_SETFL, O_RDWR | O_NONBLOCK);
      ioctl(master_fd, TIOCGPTN, &pty_idx);
      snprintf(pts_path, sizeof(pts_path), "/dev/pts%d", pty_idx);
      ws.ws_row = display_rows;
      ws.ws_col = display_cols;
      ioctl(master_fd, TIOCSWINSZ, &ws);
      shell_pid = fork();
      if (shell_pid == 0) {
        int slave_fd = open(pts_path, O_RDWR);
        if (slave_fd < 0)
          _exit(127);
        dup2(slave_fd, 0);
        dup2(slave_fd, 1);
        dup2(slave_fd, 2);
        if (slave_fd > 2)
          close(slave_fd);
        close(master_fd);
        execve("/usr/bin/shell", NULL, NULL);
        _exit(127);
      }
      linebuf_len = 0;
    }

    flush_dirty_cells();

    // Poll on libinput fd and master_fd
    struct pollfd pfds[2];
    pfds[0].fd = li_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = master_fd;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    int pr = poll(pfds, 2, -1);

    // spin 检测:li_fd 常驻可读却永无事件 = monitor pipe EOF busy-spin(§3.2)。
    // 真实事件(键/pty)复位;唯 EOF busy-spin 能累到阈值。pty 重建后 master_fd
    // 每轮重新组装,与 resume 正交(§3.3,grill Q6)。
    int real_activity = got_event || (pfds[1].revents & POLLIN);
    if (!real_activity && pr > 0 && (pfds[0].revents & POLLIN)) {
      if (++spin_count >= SPIN_THRESHOLD) {
        spin_count = 0;
        if (resume_reconnect(li) != 0) /* 路径 B:suspend→resume 重建 */
          enter_degraded_hold(li);     /* 路径 C:degraded hold + 5s 探活 */
      }
    } else {
      spin_count = 0;
    }
  }
  return 0;
}
