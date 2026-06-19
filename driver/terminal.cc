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
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/device.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/process.h>
#include "common/shm.h"
#include "common/dev.h"
#include "common/macro.h"
#include "input.h"
#include "driver/display.h"

// ===================== Shared memory (kbd only) =====================

static volatile kbd_ring *kbd;
static volatile driver_shm_header *shm_hdr;

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

    dirty_row_start = vt.rows;
    dirty_row_end = 0;

    // Mark dirty + increment generation + notify KMS if sleeping
    display_hdr->dirty_full = 1;
    display_client_flush();
}

// ===================== Main =====================

int main() {
    // 1. Initialize display client (attach display SHM from KMS)
    if (display_client_init() < 0) {
        // Unsupported bpp or no display
        while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    }

    // 2. Open KBD device, bind via REQ, then mmap SHM
    int kbd_fd;
    while ((kbd_fd = open("/dev/kbd", O_RDWR)) < 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 1);
    }

    struct kbd_req_request bind_req;
    for (int i = 0; i < 56; i++) ((uint8_t*)&bind_req)[i] = 0;
    bind_req.opcode = KBD_REQ_BIND;
    bind_req.pid = getpid();

    struct kbd_req_reply bind_reply;
    for (int i = 0; i < 64; i++) ((uint8_t*)&bind_reply)[i] = 0;

    while (1) {
        int rc = req_fd(kbd_fd, &bind_req, &bind_reply);
        if (rc == 0 && bind_reply.result == 0) break;
        struct recv_msg m;
        recv(&m, NULL, 0, 100);
    }

    // mmap kbd SHM via MAP_SHARED (fd → target_pid → sys_shm_attach)
    // size=0 is ignored — MAP_SHARED maps the entire driver SHM region
    void *shm_ptr = mmap(NULL, 0, PROT_READ | PROT_WRITE, MAP_SHARED, kbd_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    }
    uint64_t shm_addr = (uint64_t)shm_ptr;
    shm_hdr = (volatile driver_shm_header *)shm_addr;
    kbd = (volatile kbd_ring *)(shm_addr + KBD_RING_OFFSET);

    // 3. Initialize VT100 state from display SHM header
    vt.cols = display_hdr->cols;
    vt.rows = display_hdr->rows;
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
    display_client_flush();

    // 7. Create pipes for terminal ↔ shell communication
    // p_stdin:  [0]=read end (shell reads), [1]=write end (terminal writes keystrokes)
    // p_stdout: [0]=read end (terminal reads shell output), [1]=write end (shell writes)
    int p_stdin[2];
    int p_stdout[2];
    pipe(p_stdin);
    pipe(p_stdout);

    // 8. Set up fd 0 and fd 1 for shell to inherit via spawn
    // Shell fd 0 = p_stdin[0] (reads keystrokes from terminal)
    // Shell fd 1 = p_stdout[1] (writes output to terminal)
    dup2(p_stdin[0], 0);   // fd 0 = stdin pipe read end
    dup2(p_stdout[1], 1);  // fd 1 = stdout pipe write end

    // 9. Spawn shell — it inherits fd 0 (stdin read) and fd 1 (stdout write)
    {
        struct stat st;
        if (stat("/usr/bin/shell", &st) >= 0) {
            void *buf = malloc(st.st_size);
            int fd = open("/usr/bin/shell", O_RDONLY);
            if (fd >= 0 && buf) {
                read(fd, buf, st.st_size);
                close(fd);
                spawn(buf, st.st_size);
                free(buf);
            }
        }
    }

    // 10. Close pipe ends owned by shell, set up terminal's own fd 0/1
    close(p_stdin[0]);   // close read end (shell owns it)
    close(p_stdout[1]);  // close write end (shell owns it)

    // Terminal fd 0 = p_stdout[0] (read shell output, non-blocking)
    // Terminal fd 1 = p_stdin[1]  (write keystrokes to shell stdin)
    dup2(p_stdout[0], 0);
    dup2(p_stdin[1], 1);
    close(p_stdout[0]);  // close original (now duped to fd 0)
    close(p_stdin[1]);   // close original (now duped to fd 1)

    // 11. Set fd 0 to non-blocking, fd 1 to non-blocking (don't block terminal if shell isn't reading)
    fcntl(0, F_SETFL, O_RDONLY | O_NONBLOCK);
    fcntl(1, F_SETFL, O_WRONLY | O_NONBLOCK);

    // 12. Main loop
    while (1) {
        int did_work = 0;

        // Read kbd_ring → write to shell stdin pipe (fd 1)
        while (kbd->head != kbd->tail) {
            char ch = (char)kbd->msgs[kbd->tail].ch;
            kbd->tail = (kbd->tail + 1) % 8;
            write(1, &ch, 1);
            did_work = 1;
        }

        // Clear consumer_sleeping after consuming
        shm_hdr->consumer_sleeping = 0;

        // Read shell stdout pipe (fd 0) → VT100 parse → cell buffer
        char buf[256];
        int64_t n = read(0, buf, sizeof(buf));
        if (n > 0) {
            for (int64_t i = 0; i < n; i++) {
                vt100_feed(buf[i]);
            }
            did_work = 1;
        }

        // Flush dirty cells to back buffer + notify KMS
        flush_dirty_cells();

        // If no work, sleep briefly
        if (!did_work) {
            shm_hdr->consumer_sleeping = 1;
            // Double-check kbd_ring (prevent lost-wakeup)
            if (kbd->head != kbd->tail) {
                shm_hdr->consumer_sleeping = 0;
                continue;
            }
            struct recv_msg m;
            recv(&m, NULL, 0, 1);
            shm_hdr->consumer_sleeping = 0;
        }
    }

    return 0;
}
