#ifndef DRIVER_DISPLAY_H
#define DRIVER_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <sys/fb.h>
#include <sys/shm.h>
#include <sys/device.h>
#include <sys/ipc.h>
#include "common/macro.h"
#include "common/font.h"
#include "common/dev.h"

// ===== display_shm_header（KMS 和 terminal 共享）=====

struct display_shm_header {
    // 元信息（KMS 写，terminal 读）
    uint32_t fb_width;          // 像素宽
    uint32_t fb_height;         // 像素高
    uint32_t fb_pitch;          // 字节/行
    uint32_t fb_bpp;            // 位/像素（当前仅支持 32）
    uint32_t rows;              // 文本行数 (fb_height / FONT_HEIGHT)
    uint32_t cols;              // 文本列数 (fb_width / FONT_WIDTH)
    uint32_t cursor_x;          // 光标列（保留，未来硬件光标）
    uint32_t cursor_y;          // 光标行（保留，未来硬件光标）

    // dirty 追踪（terminal 写，KMS 读后清零）
    uint32_t dirty_full;        // 1 = 全屏 dirty

    // 并发同步
    uint32_t generation;        // terminal flush 时递增，KMS flip 前后校验
    uint8_t  backend_sleeping;  // KMS 设 1 后进 recv，terminal 据此决定是否 notify
    uint8_t  reserved[3];

    // 保留
    uint8_t  reserved2[28];
};

// Display SHM layout:
// page 0 (offset 0):      display_shm_header (64 bytes) + 保留 (4032 bytes)
// page 1+ (offset 4096):  back buffer (fb_height × fb_pitch 字节，页对齐)
#define DISPLAY_SHM_HEADER_SIZE 4096
#define DISPLAY_BACK_BUFFER_OFFSET DISPLAY_SHM_HEADER_SIZE

// ===== Client API（terminal 侧）=====

static struct display_shm_header *display_hdr;
static uint8_t *display_back_buffer;
static int32_t display_kms_pid;

// 初始化：attach display SHM，从 header 读 fb 元信息
static inline int display_client_init() {
    // Wait for KMS driver to register
    while ((display_kms_pid = device_lookup(DEV_KMS)) <= 0) {
        struct recv_msg m;
        recv(&m, 1);
    }

    // Attach display SHM
    void *shm_ptr = NULL;
    while (shm_attach(display_kms_pid, &shm_ptr) < 0) {
        struct recv_msg m;
        recv(&m, 1);
    }

    uint64_t shm_addr = (uint64_t)shm_ptr;
    display_hdr = (struct display_shm_header *)shm_addr;
    display_back_buffer = (uint8_t *)(shm_addr + DISPLAY_BACK_BUFFER_OFFSET);

    // Validate bpp
    if (display_hdr->fb_bpp != 32) {
        return -1;
    }

    return 0;
}

// 渲染单个 cell 到 back buffer（ch 为 ASCII，fg/bg 为 32bit 颜色）
// 纯像素渲染，不处理特殊字符（\n/\r/\b/\t 由 VT100 层处理）
static inline void display_client_render_cell(uint32_t row, uint32_t col,
                                               uint8_t ch, uint32_t fg, uint32_t bg) {
    if (ch < FONT_CHARS_START || ch > 0x7E) return;

    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;
    const uint8_t *glyph = font8x16[ch - FONT_CHARS_START];

    for (int r = 0; r < FONT_HEIGHT; r++) {
        if (py + r >= display_hdr->fb_height) break;
        uint8_t bits = glyph[r];
        uint32_t *dst = (uint32_t *)(display_back_buffer + (py + r) * display_hdr->fb_pitch + px * 4);
        for (int c = 0; c < FONT_WIDTH; c++) {
            if (px + c >= display_hdr->fb_width) break;
            dst[c] = (bits & (0x80 >> c)) ? fg : bg;
        }
    }
}

// 清空 back buffer（填 bg 颜色），标记 dirty_full
static inline void display_client_clear(uint32_t bg) {
    uint32_t total_pixels = display_hdr->fb_height * (display_hdr->fb_pitch / 4);
    uint32_t *buf = (uint32_t *)display_back_buffer;
    for (uint32_t i = 0; i < total_pixels; i++) {
        buf[i] = bg;
    }
    display_hdr->dirty_full = 1;
}

// 在 back buffer 上执行 scroll（memmove 向上搬像素行，uint32_t 循环清末行）
static inline void display_client_scroll_up(uint32_t bg) {
    uint32_t line_bytes = display_hdr->fb_pitch * FONT_HEIGHT;
    uint32_t fb_bytes = display_hdr->fb_height * display_hdr->fb_pitch;

    // Shift up by one text line
    uint8_t *buf = display_back_buffer;
    for (uint32_t i = 0; i < fb_bytes - line_bytes; i++) {
        buf[i] = buf[i + line_bytes];
    }

    // Clear last text line with bg color (pixel by pixel, 32bpp)
    uint32_t *last_line = (uint32_t *)(buf + fb_bytes - line_bytes);
    uint32_t pixels_per_line = line_bytes / 4;
    for (uint32_t i = 0; i < pixels_per_line; i++) {
        last_line[i] = bg;
    }

    display_hdr->dirty_full = 1;
}

// 设置光标位置（更新 header 元信息，供未来硬件光标）
static inline void display_client_set_cursor(uint32_t x, uint32_t y) {
    display_hdr->cursor_x = x;
    display_hdr->cursor_y = y;
}

// 标记 dirty_full + 递增 generation + 检查 backend_sleeping 决定是否 notify
// 内部守卫：dirty_full==0 时直接返回，不递增 generation 也不 notify
static inline void display_client_flush() {
    if (display_hdr->dirty_full == 0) return;
    display_hdr->generation++;
    if (display_hdr->backend_sleeping) {
        notify(display_kms_pid);
    }
}

// ===== Backend API（KMS 侧）=====

static struct display_shm_header *backend_hdr;
static uint8_t *backend_back_buffer;
static uint8_t *backend_front_buffer;  // directly mapped framebuffer

// 初始化：sys_fb_info 获取 fb 元信息 → sys_shm_create 创建 display SHM →
// 写入 header 元信息 → 注册 DEV_KMS
static inline int display_backend_init() {
    // Get framebuffer info
    struct kms_fb_info kfb;
    fb_info(&kfb);

    if (kfb.width == 0 || kfb.fb_vaddr == 0) {
        return -1;
    }

    backend_front_buffer = (uint8_t *)(uintptr_t)kfb.fb_vaddr;

    // Calculate SHM size: header page + back buffer pages
    uint32_t back_buffer_size = kfb.height * kfb.pitch;
    uint32_t shm_size = DISPLAY_SHM_HEADER_SIZE + ALIGN_UP(back_buffer_size, 4096);

    // Create display SHM
    void *shm_ptr = NULL;
    if (shm_create(shm_size, &shm_ptr) < 0) {
        return -1;
    }

    uint64_t shm_addr = (uint64_t)shm_ptr;
    backend_hdr = (struct display_shm_header *)shm_addr;
    backend_back_buffer = (uint8_t *)(shm_addr + DISPLAY_BACK_BUFFER_OFFSET);

    // Write header metadata
    backend_hdr->fb_width  = kfb.width;
    backend_hdr->fb_height = kfb.height;
    backend_hdr->fb_pitch  = kfb.pitch;
    backend_hdr->fb_bpp    = kfb.bpp;
    backend_hdr->rows      = kfb.height / FONT_HEIGHT;
    backend_hdr->cols      = kfb.width / FONT_WIDTH;
    backend_hdr->cursor_x  = 0;
    backend_hdr->cursor_y  = 0;
    backend_hdr->dirty_full = 0;
    backend_hdr->generation = 0;
    backend_hdr->backend_sleeping = 0;

    // Clear back buffer to black
    uint32_t total_pixels = kfb.height * (kfb.pitch / 4);
    uint32_t *buf = (uint32_t *)backend_back_buffer;
    for (uint32_t i = 0; i < total_pixels; i++) {
        buf[i] = 0;
    }

    // Clear front buffer to black
    uint32_t *front = (uint32_t *)backend_front_buffer;
    for (uint32_t i = 0; i < total_pixels; i++) {
        front[i] = 0;
    }

    // Register as DEV_KMS
    device_register(sys_getpid(), DEV_KMS);

    return 0;
}

// 轮询：检查 dirty_full，memcpy back → front，验证 generation
// 返回是否有工作（1=处理了 dirty，0=无 dirty）
static inline int display_backend_poll() {
    uint32_t g1 = backend_hdr->generation;

    if (backend_hdr->dirty_full != 1) return 0;

    // memcpy back buffer → front buffer
    uint32_t fb_bytes = backend_hdr->fb_height * backend_hdr->fb_pitch;
    uint8_t *front = backend_front_buffer;
    uint8_t *back  = backend_back_buffer;
    for (uint32_t i = 0; i < fb_bytes; i++) {
        front[i] = back[i];
    }

    uint32_t g2 = backend_hdr->generation;
    if (g1 == g2) {
        backend_hdr->dirty_full = 0;
    }
    // If g1 != g2, don't clear dirty — main loop will continue and re-poll
    return 1;
}

// 等待：设 backend_sleeping=1 → double-check dirty → recv 超时等待
static inline void display_backend_wait(uint32_t timeout_ms) {
    backend_hdr->backend_sleeping = 1;
    // Double-check: if dirty appeared after we set sleeping, abort sleep
    if (backend_hdr->dirty_full == 1) {
        backend_hdr->backend_sleeping = 0;
        return;
    }
    struct recv_msg msg;
    recv(&msg, timeout_ms);
    backend_hdr->backend_sleeping = 0;
}

#endif
