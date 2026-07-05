#ifndef DRIVER_DISPLAY_H
#define DRIVER_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <fcntl.h>
#include <unistd.h>
#include "user/driver/font.h"
#include "xos/display.h"

// ===== ioctl commands from <sys/ioctl.h> (xos/ioctl.h) =====

// ===== Client API (compositor side) =====

// Local metadata (from CREATE_BUF response)
static uint32_t display_pitch;
static uint32_t display_fb_width;
static uint32_t display_fb_height;
static uint32_t display_rows;
static uint32_t display_cols;
static uint8_t *display_back_buffer;
static int display_dev_fd;

// Initialize: open("/dev/kms") + ioctl(CREATE_BUF) + mmap(fd)
static inline int display_client_init() {
    int fd;
    printf("display_client_init: opening /dev/kms\n");
    while ((fd = open("/dev/kms", O_RDWR)) < 0) {
        printf("display_client_init: open failed, recv wait\n");
        struct recv_msg m;
        recv(&m, NULL, 0, 1);
    }
    printf("display_client_init: /dev/kms opened fd=%d\n", fd);
    display_dev_fd = fd;

    // Send CREATE_BUF via ioctl
    struct display_ioctl_create_buf_arg arg;
    for (int i = 0; i < (int)sizeof(arg); i++) ((uint8_t*)&arg)[i] = 0;
    arg.width = 800;
    arg.height = 600;
    arg.bpp = 32;

    printf("display_client_init: calling ioctl CREATE_BUF\n");
    int rc = ioctl(fd, KMS_IOCTL_CREATE_BUF, &arg);
    printf("display_client_init: ioctl returned rc=%d\n", rc);
    if (rc < 0) return -1;

    // Read response from arg (kernel fills output fields via copy_to_user)
    if (arg.result != 0) return -1;

    display_pitch = arg.pitch;
    display_fb_width = 800;
    display_fb_height = 600;
    display_rows = arg.rows;
    display_cols = arg.cols;

    // mmap back buffer
    printf("display_client_init: calling mmap size=%d\n", arg.size);
    void *buf = mmap(NULL, arg.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    printf("display_client_init: mmap returned buf=%p\n", buf);
    if (buf == MAP_FAILED) return -1;

    display_back_buffer = (uint8_t *)buf;
    return 0;
}

// Render a single cell to back buffer
static inline void display_client_render_cell(uint32_t row, uint32_t col,
                                               uint8_t ch, uint32_t fg, uint32_t bg) {
    if (ch < FONT_CHARS_START || ch > 0x7E) return;

    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;
    const uint8_t *glyph = font8x16[ch - FONT_CHARS_START];

    // cols × FONT_WIDTH = fb_width，px+c 不会越界，移除内层边界检查。
    // 预计算每行的 fg/bg 指针，内层循环只剩位测试 + 条件写。
    for (int r = 0; r < FONT_HEIGHT; r++) {
        if (py + r >= display_fb_height) break;
        uint8_t bits = glyph[r];
        uint32_t *dst = (uint32_t *)(display_back_buffer + (py + r) * display_pitch + px * 4);
        for (int c = 0; c < FONT_WIDTH; c++) {
            dst[c] = (bits & (0x80 >> c)) ? fg : bg;
        }
    }
}

// Clear back buffer
static inline void display_client_clear(uint32_t bg) {
    uint32_t total_pixels = display_fb_height * (display_pitch / 4);
    uint32_t *buf = (uint32_t *)display_back_buffer;
    for (uint32_t i = 0; i < total_pixels; i++) {
        buf[i] = bg;
    }
}

// Scroll back buffer
static inline void display_client_scroll_up(uint32_t bg) {
    uint32_t line_bytes = display_pitch * FONT_HEIGHT;
    uint32_t fb_bytes = display_fb_height * display_pitch;
    uint32_t move_bytes = fb_bytes - line_bytes;

    // uint64_t 搬运（8 字节步进），尾部按字节兜底。
    // display_pitch (3200) 和 line_bytes (51200) 均为 8 的倍数，正常无尾部。
    uint64_t *dst64 = (uint64_t *)display_back_buffer;
    const uint64_t *src64 = (const uint64_t *)(display_back_buffer + line_bytes);
    uint32_t n64 = move_bytes / 8;
    for (uint32_t i = 0; i < n64; i++) dst64[i] = src64[i];

    uint32_t tail = move_bytes - n64 * 8;
    if (tail) {
        uint8_t *d = display_back_buffer + n64 * 8;
        const uint8_t *s = display_back_buffer + line_bytes + n64 * 8;
        for (uint32_t i = 0; i < tail; i++) d[i] = s[i];
    }

    uint32_t *last_line = (uint32_t *)(display_back_buffer + fb_bytes - line_bytes);
    uint32_t pixels_per_line = line_bytes / 4;
    for (uint32_t i = 0; i < pixels_per_line; i++) {
        last_line[i] = bg;
    }
}

// Cursor (no-op: kernel KMS doesn't know about cursor)
static inline void display_client_set_cursor(uint32_t x, uint32_t y) {
    (void)x; (void)y;
}

// Request kernel flip: ioctl(fd, FLIP, dirty_range)
static inline void display_client_flush(uint32_t dirty_row_start,
                                         uint32_t dirty_row_end) {
    struct display_ioctl_flip_arg arg;
    arg.dirty_row_start = dirty_row_start;
    arg.dirty_row_end   = dirty_row_end;
    ioctl(display_dev_fd, KMS_IOCTL_FLIP, &arg);
}

#endif
