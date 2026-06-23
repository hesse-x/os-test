#ifndef DRIVER_DISPLAY_H
#define DRIVER_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <fcntl.h>
#include <unistd.h>
#include "driver/font.h"
#include "common/dev.h"

// ===== Display request constants =====
#define DISPLAY_REQ_CREATE_BUF  1
#define DISPLAY_REQ_FLIP        2

// ===== Request/Response structures =====
struct display_create_buf_req {
    uint32_t req_type;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
};

struct display_create_buf_resp {
    uint32_t pitch;
    uint32_t size;
    uint32_t rows;
    uint32_t cols;
    int32_t  result;
};

struct display_flip_resp {
    int32_t result;
};

// ===== Client API (compositor side) =====

// Local metadata (from CREATE_BUF response)
static uint32_t display_pitch;
static uint32_t display_fb_width;
static uint32_t display_fb_height;
static uint32_t display_rows;
static uint32_t display_cols;
static uint8_t *display_back_buffer;
static int display_dev_fd;

// Initialize: open("/dev/kms") + req(CREATE_BUF) + mmap(fd)
static inline int display_client_init() {
    int fd;
    while ((fd = open("/dev/kms", O_RDWR)) < 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 1);
    }
    display_dev_fd = fd;

    // Send CREATE_BUF request
    uint8_t req_buf[56];
    uint8_t resp_buf[64];
    for (int i = 0; i < 56; i++) req_buf[i] = 0;
    for (int i = 0; i < 64; i++) resp_buf[i] = 0;

    struct display_create_buf_req *req = (struct display_create_buf_req *)req_buf;
    req->req_type = DISPLAY_REQ_CREATE_BUF;
    req->width = 800;
    req->height = 600;
    req->bpp = 32;

    int rc = req_fd(fd, req_buf, resp_buf);
    if (rc != 0) return -1;

    struct display_create_buf_resp *resp = (struct display_create_buf_resp *)resp_buf;
    if (resp->result != 0) return -1;

    display_pitch = resp->pitch;
    display_fb_width = 800;
    display_fb_height = 600;
    display_rows = resp->rows;
    display_cols = resp->cols;

    // mmap back buffer
    void *buf = mmap(NULL, resp->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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

    for (int r = 0; r < FONT_HEIGHT; r++) {
        if (py + r >= display_fb_height) break;
        uint8_t bits = glyph[r];
        uint32_t *dst = (uint32_t *)(display_back_buffer + (py + r) * display_pitch + px * 4);
        for (int c = 0; c < FONT_WIDTH; c++) {
            if (px + c >= display_fb_width) break;
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

    uint8_t *buf = display_back_buffer;
    for (uint32_t i = 0; i < fb_bytes - line_bytes; i++) {
        buf[i] = buf[i + line_bytes];
    }

    uint32_t *last_line = (uint32_t *)(buf + fb_bytes - line_bytes);
    uint32_t pixels_per_line = line_bytes / 4;
    for (uint32_t i = 0; i < pixels_per_line; i++) {
        last_line[i] = bg;
    }
}

// Cursor (no-op: kernel KMS doesn't know about cursor)
static inline void display_client_set_cursor(uint32_t x, uint32_t y) {
    (void)x; (void)y;
}

// Request kernel flip: req(fd, FLIP)
static inline void display_client_flush() {
    uint8_t req_buf[56];
    uint8_t resp_buf[64];
    for (int i = 0; i < 56; i++) req_buf[i] = 0;
    for (int i = 0; i < 64; i++) resp_buf[i] = 0;

    *(uint32_t *)req_buf = DISPLAY_REQ_FLIP;  // FLIP has no payload, req_type at offset 0 is correct

    req_fd(display_dev_fd, req_buf, resp_buf);
}

#endif
