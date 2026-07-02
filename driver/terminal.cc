// Terminal process: VT100 state machine + cell buffer + compositor.
// Renders cells → pixels → back buffer (display SHM), KMS does flip.
//
// fd 0 = stdout pipe read end (reads shell output, O_NONBLOCK)
// fd 1 = stdin pipe write end  (sends keystrokes to shell)
//
// Links libc.a, uses main() entry point.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/process.h>
#include <sys/poll.h>
#include "common/macro.h"
#include "input.h"
#include "common/input.h"
#include "driver/display.h"

static int master_fd = -1;
static pid_t shell_pid = -1;

// ===================== VT100 state =====================

#define VT100_NORMAL  0
#define VT100_ESC    1
#define VT100_CSI    2

struct cell {
    uint8_t  ch;
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

// ===================== VT100 color table =====================

static const uint32_t vt100_colors[] = {
    0x000000, // 0: black
    0x800000, // 1: red
    0x008000, // 2: green
    0x808000, // 3: yellow
    0x000080, // 4: blue
    0x800080, // 5: magenta
    0x008080, // 6: cyan
    0xC0C0C0, // 7: white (light gray)
    0x808080, // 8: bright black (dark gray)
    0xFF0000, // 9: bright red
    0x00FF00, // 10: bright green
    0xFFFF00, // 11: bright yellow
    0x0000FF, // 12: bright blue
    0xFF00FF, // 13: bright magenta
    0x00FFFF, // 14: bright cyan
    0xFFFFFF, // 15: bright white
};

// ===================== Cell buffer helpers =====================

static void mark_dirty(int row) {
    if (row < dirty_row_start) dirty_row_start = row;
    if (row + 1 > dirty_row_end) dirty_row_end = row + 1;
}

static void cell_putc(char c) {
    if (c == '\n') {
        vt.cursor_x = 0;
        if (vt.cursor_y < vt.rows - 1) {
            vt.cursor_y++;
        } else {
            // Scroll: shift cells up one row
            for (int r = 0; r < vt.rows - 1; r++) {
                for (int col = 0; col < vt.cols; col++) {
                    cells[r * vt.cols + col] = cells[(r + 1) * vt.cols + col];
                }
            }
            // Clear last row
            for (int col = 0; col < vt.cols; col++) {
                cells[(vt.rows - 1) * vt.cols + col].ch = ' ';
                cells[(vt.rows - 1) * vt.cols + col].fg_color = vt.fg_color;
                cells[(vt.rows - 1) * vt.cols + col].bg_color = vt.bg_color;
            }
            dirty_row_start = 0;
            dirty_row_end = vt.rows;
            // Scroll back buffer
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
            if (vt.cursor_y < vt.rows - 1) vt.cursor_y++;
        } else {
            vt.cursor_x = next_tab;
        }
        return;
    }

    // Normal printable char
    if (vt.cursor_x >= vt.cols) {
        vt.cursor_x = 0;
        if (vt.cursor_y < vt.rows - 1) {
            vt.cursor_y++;
        } else {
            // Scroll (same as \n at bottom)
            for (int r = 0; r < vt.rows - 1; r++) {
                for (int col = 0; col < vt.cols; col++) {
                    cells[r * vt.cols + col] = cells[(r + 1) * vt.cols + col];
                }
            }
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

// ===================== VT100 parser =====================

static int csi_param(int idx, int default_val) {
    if (idx < vt.csi_param_count && vt.csi_params[idx] > 0)
        return vt.csi_params[idx];
    return default_val;
}

static void vt100_csi_dispatch(char final_ch) {
    switch (final_ch) {
    case 'H': case 'f': { // Cursor position: \033[row;colH
        int row = csi_param(0, 1) - 1;
        int col = csi_param(1, 1) - 1;
        if (row < 0) row = 0;
        if (row >= vt.rows) row = vt.rows - 1;
        if (col < 0) col = 0;
        if (col >= vt.cols) col = vt.cols - 1;
        vt.cursor_y = row;
        vt.cursor_x = col;
        break;
    }
    case 'J': { // Erase in display: \033[2J = clear screen
        int mode = csi_param(0, 0);
        if (mode == 2) {
            // Clear entire screen
            for (int r = 0; r < vt.rows; r++) {
                for (int c = 0; c < vt.cols; c++) {
                    cells[r * vt.cols + c].ch = ' ';
                    cells[r * vt.cols + c].fg_color = vt.fg_color;
                    cells[r * vt.cols + c].bg_color = vt.bg_color;
                }
            }
            dirty_row_start = 0;
            dirty_row_end = vt.rows;
            vt.cursor_x = 0;
            vt.cursor_y = 0;
            display_client_clear(vt.bg_color);
        }
        break;
    }
    case 'K': { // Erase in line: \033[K = clear from cursor to end
        for (int c = vt.cursor_x; c < vt.cols; c++) {
            struct cell *ce = &cells[vt.cursor_y * vt.cols + c];
            ce->ch = ' ';
            ce->fg_color = vt.fg_color;
            ce->bg_color = vt.bg_color;
        }
        mark_dirty(vt.cursor_y);
        break;
    }
    case 'm': { // SGR (Select Graphic Rendition)
        if (vt.csi_param_count == 0) {
            // \033[m = reset
            vt.fg_color = 0xFFFFFF;
            vt.bg_color = 0x000000;
        } else {
            for (int i = 0; i < vt.csi_param_count; i++) {
                int p = vt.csi_params[i];
                if (p == 0) {
                    vt.fg_color = 0xFFFFFF;
                    vt.bg_color = 0x000000;
                } else if (p >= 30 && p <= 37) {
                    vt.fg_color = vt100_colors[p - 30];
                } else if (p >= 40 && p <= 47) {
                    vt.bg_color = vt100_colors[p - 40];
                } else if (p >= 90 && p <= 97) {
                    vt.fg_color = vt100_colors[p - 90 + 8];
                } else if (p >= 100 && p <= 107) {
                    vt.bg_color = vt100_colors[p - 100 + 8];
                }
            }
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
        if (c == 0x1B) {
            vt.escape_state = VT100_ESC;
        } else {
            cell_putc(c);
        }
        break;
    case VT100_ESC:
        if (c == '[') {
            vt.escape_state = VT100_CSI;
            vt.csi_param_count = 0;
            for (int i = 0; i < 8; i++) vt.csi_params[i] = 0;
        } else {
            // Unknown escape, treat as normal char
            cell_putc(c);
            vt.escape_state = VT100_NORMAL;
        }
        break;
    case VT100_CSI:
        if (c >= '0' && c <= '9') {
            if (vt.csi_param_count < 8) {
                vt.csi_params[vt.csi_param_count] =
                    vt.csi_params[vt.csi_param_count] * 10 + (c - '0');
            }
        } else if (c == ';') {
            vt.csi_param_count++;
        } else if (c >= 0x40 && c <= 0x7E) {
            // Final byte: dispatch
            vt.csi_param_count++;  // include last param
            vt100_csi_dispatch(c);
            vt.escape_state = VT100_NORMAL;
        } else {
            // Intermediate byte or unknown — ignore
        }
        break;
    }
}

// ===================== Dirty cell flush to back buffer =====================

static void flush_dirty_cells() {
    if (dirty_row_start >= dirty_row_end) return;

    for (int row = dirty_row_start; row < dirty_row_end; row++) {
        for (int col = 0; col < vt.cols; col++) {
            struct cell *c = &cells[row * vt.cols + col];
            display_client_render_cell(row, col, c->ch, c->fg_color, c->bg_color);
        }
    }

    // Update cursor position in display header
    display_client_set_cursor(vt.cursor_x, vt.cursor_y);

    int rs = dirty_row_start;
    int re = dirty_row_end;
    dirty_row_start = vt.rows;
    dirty_row_end = 0;

    // Flush to kernel KMS with dirty row range
    display_client_flush(rs, re);
}

// ===================== Main =====================

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;
    // 1. Initialize display client (attach display SHM from KMS)
    if (display_client_init() < 0) {
        printf("terminal: display_client_init FAILED\n");
        while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    }

    // 2. Open /dev/kbd, bind (register pid for notify), mmap driver's SHM via inode.
    //    Direction A: driver owns SHM and bound it to /dev/kbd inode via
    //    device_register_shm. Consumer accesses via open + mmap(MAP_SHARED, fd).
    int kbd_fd;
    while ((kbd_fd = open("/dev/kbd", O_RDWR)) < 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 1);
    }

    // BIND: register consumer pid (Direction A — no shm_fd passing).
    struct input_bind_arg bind_arg;
    bind_arg.shm_fd = -1;
    bind_arg.result = -1;
    while (1) {
        int rc = ioctl(kbd_fd, INPUT_BIND, &bind_arg);
        if (rc == 0 && bind_arg.result == 0) break;
        struct recv_msg m;
        recv(&m, NULL, 0, 100);
        bind_arg.shm_fd = -1;
        bind_arg.result = -1;
    }

    // Access driver's SHM through /dev/kbd inode (kernel sys_mmap FD_DEV + ip->shm path).
    volatile void *input_shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, kbd_fd, 0);
    if (input_shm == MAP_FAILED) {
        printf("terminal: mmap /dev/kbd SHM FAILED\n");
        while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    }

    // 3. Initialize VT100 state from display metadata
    vt.cols = display_cols;
    vt.rows = display_rows;
    vt.cursor_x = 0;
    vt.cursor_y = 0;
    vt.fg_color = 0xFFFFFF;
    vt.bg_color = 0x000000;
    vt.escape_state = VT100_NORMAL;
    vt.csi_param_count = 0;

    // 4. Allocate cell buffer via mmap
    int cell_bytes = vt.rows * vt.cols * sizeof(struct cell);
    cells = (struct cell *)mmap(NULL, cell_bytes, PROT_READ | PROT_WRITE, 0, -1, 0);
    if (!cells) {
        printf("terminal: mmap cells FAILED\n");
        while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    }

    // 5. Initialize cells to spaces
    for (int r = 0; r < vt.rows; r++) {
        for (int c = 0; c < vt.cols; c++) {
            cells[r * vt.cols + c].ch = ' ';
            cells[r * vt.cols + c].fg_color = 0xFFFFFF;
            cells[r * vt.cols + c].bg_color = 0x000000;
        }
    }
    dirty_row_start = vt.rows;
    dirty_row_end = 0;

    // 6. Clear screen (back buffer)
    display_client_clear(0x000000);
    display_client_flush(0, display_rows);

    // 7. Create PTY pair
    master_fd = open("/dev/ptmx", O_RDWR);
    if (master_fd < 0) { printf("terminal: failed to open /dev/ptmx\n"); return 1; }

    // 8. Get PTY index for dynamic slave path
    int pty_idx;
    ioctl(master_fd, TIOCGPTN, &pty_idx);
    char pts_path[16];
    snprintf(pts_path, sizeof(pts_path), "/dev/pts%d", pty_idx);

    // 9. Fork shell — child opens slave, dup2 0/1/2, exec
    shell_pid = fork();
    if (shell_pid == 0) {
        int slave_fd = open(pts_path, O_RDWR);
        if (slave_fd < 0) { write(2, "shell: failed to open slave\n", 29); _exit(127); }
        dup2(slave_fd, 0); dup2(slave_fd, 1); dup2(slave_fd, 2);
        if (slave_fd > 2) close(slave_fd);
        close(master_fd);
        execve("/usr/bin/shell", NULL, NULL);
        write(2, "shell_child: execve FAILED\n", 28);
        _exit(127);
    }

    // 10. Parent: master_fd non-blocking
    fcntl(master_fd, F_SETFL, O_RDWR | O_NONBLOCK);

    // 11. Set PTY winsize to actual display dimensions
    struct winsize ws;
    ws.ws_row = display_rows;
    ws.ws_col = display_cols;
    ws.ws_xpixel = 0; ws.ws_ypixel = 0;
    ioctl(master_fd, TIOCSWINSZ, &ws);

    // 12. Main loop — PTY master holder + line discipline
    char linebuf[256];
    int linebuf_len = 0;

    while (1) {
        int did_work = 0;
        struct termios t;
        ioctl(master_fd, TCGETS, &t);
        // input_event → ASCII → ldisc → master write
        input_event_t evs[64];
        int nev = input_client_poll(input_shm, evs, 64);
        for (int ei = 0; ei < nev; ei++) {
            uint8_t ascii_buf[4];
            int ascii_len = input_event_to_ascii(&evs[ei], ascii_buf, sizeof(ascii_buf));
            if (ascii_len <= 0) continue;  // non-ASCII (modifier/extended) — ldisc handles via caller
            char ch = (char)ascii_buf[0];

            // Get foreground pgid for signal delivery
            pid_t fg_pgid = shell_pid;
            int tmp_pgid;
            if (ioctl(master_fd, TIOCGPGRP, &tmp_pgid) == 0 && tmp_pgid > 0)
                fg_pgid = tmp_pgid;

            if ((t.c_lflag & ISIG) && ch == (char)t.c_cc[VINTR]) {
                kill(-fg_pgid, SIGINT); did_work = 1; continue;
            }
            if ((t.c_lflag & ISIG) && ch == (char)t.c_cc[VSUSP]) {
                kill(-fg_pgid, SIGTSTP); did_work = 1; continue;
            }

            if (t.c_lflag & ICANON) {
                if (ch == '\n') {
                    linebuf[linebuf_len++] = '\n';
                    write(master_fd, linebuf, linebuf_len);
                    vt100_feed('\r'); vt100_feed('\n');
                    linebuf_len = 0; did_work = 1; continue;
                }
                if (ch == (char)t.c_cc[VEOF]) {
                    if (linebuf_len > 0) {
                        write(master_fd, linebuf, linebuf_len);
                        linebuf_len = 0; did_work = 1;
                    } else {
                        write(master_fd, "", 0);  // eof_pending → slave EOF
                        did_work = 1;
                    }
                    continue;
                }
                if (ch == (char)t.c_cc[VERASE]) {
                    if (linebuf_len > 0) {
                        linebuf_len--;
                        vt100_feed('\b'); vt100_feed(' '); vt100_feed('\b');
                        did_work = 1;
                    }
                    continue;
                }
                if (ch == (char)t.c_cc[VKILL]) {
                    linebuf_len = 0;
                    vt100_feed('\r'); vt100_feed('\n');
                    did_work = 1; continue;
                }
                if (linebuf_len < (int)sizeof(linebuf) - 2) {
                    linebuf[linebuf_len++] = ch;
                    if (t.c_lflag & ECHO) vt100_feed(ch);
                    did_work = 1;
                }
                continue;
            }

            // Raw mode
            write(master_fd, &ch, 1);
            if (t.c_lflag & ECHO) vt100_feed(ch);
            did_work = 1;
        }

        // Shell output ← master read → VT100 + serial echo
        char buf[4096];
        int64_t n = read(master_fd, buf, sizeof(buf));
        if (n > 0) {
            write(2, buf, (size_t)n);
            for (int64_t i = 0; i < n; i++) {
                vt100_feed(buf[i]);
                // Flush every few dirty rows to keep display responsive
                if (dirty_row_end - dirty_row_start >= 4)
                    flush_dirty_cells();
            }
            did_work = 1;
        } else if (n == 0) {
            // Shell exited → re-fork
            printf("terminal: shell exited (n=0), re-forking\n");
            close(master_fd);
            master_fd = open("/dev/ptmx", O_RDWR);
            if (master_fd < 0) continue;
            fcntl(master_fd, F_SETFL, O_RDWR | O_NONBLOCK);
            ioctl(master_fd, TIOCGPTN, &pty_idx);
            snprintf(pts_path, sizeof(pts_path), "/dev/pts%d", pty_idx);
            ws.ws_row = display_rows; ws.ws_col = display_cols;
            ioctl(master_fd, TIOCSWINSZ, &ws);
            shell_pid = fork();
            if (shell_pid == 0) {
                int slave_fd = open(pts_path, O_RDWR);
                if (slave_fd < 0) _exit(127);
                dup2(slave_fd, 0); dup2(slave_fd, 1); dup2(slave_fd, 2);
                if (slave_fd > 2) close(slave_fd);
                close(master_fd);
                execve("/usr/bin/shell", NULL, NULL);
                _exit(127);
            }
            linebuf_len = 0; did_work = 1;
        }

        flush_dirty_cells();

        // 用 poll 替代 recv 超时：阻塞等 master_fd 可读，10ms 超时回来检查键盘 SHM。
        // 键盘 SHM 无 notify 机制，只能轮询；10ms 超时平衡键盘延迟与 CPU 占用。
        // shell 写数据 → slave_write → wake_process(master_owner_pid) 立刻唤醒。
        struct pollfd pfd = { .fd = master_fd, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, 10);
    }
    return 0;
}
