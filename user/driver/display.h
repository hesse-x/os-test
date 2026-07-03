#ifndef DRIVER_DISPLAY_H
#define DRIVER_DISPLAY_H

/* Userspace display client API — DRM/KMS backend (virtio-gpu).
   API surface unchanged from the old bochs-display KMS version:
   terminal.cc calls display_client_init / render_cell / clear /
   scroll_up / set_cursor / flush. Internals now drive DRM ioctls
   on /dev/dri/card0 (CREATE_DUMB / MAP_DUMB / mmap / ADDFB /
   SETCRTC / PAGE_FLIP). */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <fcntl.h>
#include <unistd.h>
#include "user/driver/font.h"
#include "utils/drm.h"

/* Local metadata (filled by display_client_init) */
static uint32_t display_pitch;
static uint32_t display_fb_width;
static uint32_t display_fb_height;
static uint32_t display_rows;
static uint32_t display_cols;
static uint8_t *display_back_buffer;
static int display_dev_fd;

/* DRM handles (internal) */
static uint32_t drm_dumb_handle;
static uint32_t drm_fb_id;

/* Initialize: open /dev/dri/card0, CREATE_DUMB, MAP_DUMB + mmap,
   ADDFB, SETCRTC. Mirrors the old open("/dev/kms") + CREATE_BUF +
   mmap(fd, 0) sequence so terminal.cc is unchanged. */
static inline int display_client_init(void) {
    int fd;
    printf("display_client_init: opening /dev/dri/card0\n");
    while ((fd = open("/dev/dri/card0", O_RDWR)) < 0) {
        printf("display_client_init: open failed, recv wait\n");
        struct recv_msg m;
        recv(&m, NULL, 0, 1);
    }
    printf("display_client_init: /dev/dri/card0 opened fd=%d\n", fd);
    display_dev_fd = fd;

    /* SET_MASTER (best-effort; kernel allows it) */
    ioctl(fd, DRM_IOCTL_SET_MASTER, 0);

    /* Query the supported mode from the kernel (single fixed mode), so the
       client follows the kernel's runtime default rather than hardcoding. */
    struct drm_mode_card_res res;
    for (int i = 0; i < (int)sizeof(res); i++) ((uint8_t *)&res)[i] = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || !res.max_width || !res.max_height) {
        printf("display_client_init: GETRESOURCES failed\n");
        return -1;
    }

    /* CREATE_DUMB: use the kernel's current mode (res.max_width/height). */
    struct drm_mode_create_dumb dumb;
    for (int i = 0; i < (int)sizeof(dumb); i++) ((uint8_t *)&dumb)[i] = 0;
    dumb.width = res.max_width;
    dumb.height = res.max_height;
    dumb.bpp = 32;

    printf("display_client_init: calling ioctl CREATE_DUMB\n");
    int rc = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
    printf("display_client_init: CREATE_DUMB rc=%d handle=%u pitch=%u size=%llu\n",
           rc, dumb.handle, dumb.pitch, (unsigned long long)dumb.size);
    if (rc < 0) return -1;

    drm_dumb_handle = dumb.handle;
    display_pitch = dumb.pitch;
    display_fb_width = dumb.width;
    display_fb_height = dumb.height;
    display_rows = display_fb_height / FONT_HEIGHT;
    display_cols = display_fb_width / FONT_WIDTH;

    /* MAP_DUMB: get an offset to pass to mmap */
    struct drm_mode_map_dumb map;
    for (int i = 0; i < (int)sizeof(map); i++) ((uint8_t *)&map)[i] = 0;
    map.handle = drm_dumb_handle;
    rc = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    printf("display_client_init: MAP_DUMB rc=%d offset=%llu\n",
           rc, (unsigned long long)map.offset);
    if (rc < 0) return -1;

    /* mmap back buffer. Kernel drm_mmap_handler matches by size, so the
       length must equal dumb.size. */
    printf("display_client_init: calling mmap size=%llu\n",
           (unsigned long long)dumb.size);
    void *buf = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, map.offset);
    printf("display_client_init: mmap returned buf=%p\n", buf);
    if (buf == MAP_FAILED) return -1;
    display_back_buffer = (uint8_t *)buf;

    /* ADDFB: wrap the dumb buffer in a framebuffer */
    struct drm_mode_fb_cmd fb;
    for (int i = 0; i < (int)sizeof(fb); i++) ((uint8_t *)&fb)[i] = 0;
    fb.width = display_fb_width;
    fb.height = display_fb_height;
    fb.pitch = display_pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = drm_dumb_handle;
    rc = ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb);
    printf("display_client_init: ADDFB rc=%d fb_id=%u\n", rc, fb.fb_id);
    if (rc < 0) return -1;
    drm_fb_id = fb.fb_id;

    /* SETCRTC: bind framebuffer to CRTC 1 with the fixed 800x600 mode */
    struct drm_mode_crtc crtc;
    for (int i = 0; i < (int)sizeof(crtc); i++) ((uint8_t *)&crtc)[i] = 0;
    crtc.crtc_id = 1;  /* DRM_CRTC_ID */
    crtc.fb_id = drm_fb_id;
    crtc.mode_valid = 1;
    crtc.mode.hdisplay = display_fb_width;
    crtc.mode.vdisplay = display_fb_height;
    crtc.mode.vrefresh = 60;
    rc = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
    printf("display_client_init: SETCRTC rc=%d\n", rc);
    if (rc < 0) return -1;

    return 0;
}

/* Render a single cell to the back buffer */
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
            dst[c] = (bits & (0x80 >> c)) ? fg : bg;
        }
    }
}

/* Clear back buffer */
static inline void display_client_clear(uint32_t bg) {
    uint32_t total_pixels = display_fb_height * (display_pitch / 4);
    uint32_t *buf = (uint32_t *)display_back_buffer;
    for (uint32_t i = 0; i < total_pixels; i++) {
        buf[i] = bg;
    }
}

/* Scroll back buffer up by one text row */
static inline void display_client_scroll_up(uint32_t bg) {
    uint32_t line_bytes = display_pitch * FONT_HEIGHT;
    uint32_t fb_bytes = display_fb_height * display_pitch;
    uint32_t move_bytes = fb_bytes - line_bytes;

    /* uint64_t 搬运（8 字节步进），尾部按字节兜底 */
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

/* Cursor (no-op: software cursor rendered by terminal) */
static inline void display_client_set_cursor(uint32_t x, uint32_t y) {
    (void)x; (void)y;
}

/* Flush: page-flip the back buffer to the scanout. The dirty-row range
   is ignored for now (the kernel transfers the full frame on flip);
   keeping the argument preserves the call sites in terminal.cc. */
static inline void display_client_flush(uint32_t dirty_row_start,
                                        uint32_t dirty_row_end) {
    (void)dirty_row_start;
    (void)dirty_row_end;
    struct drm_mode_crtc_page_flip flip;
    for (int i = 0; i < (int)sizeof(flip); i++) ((uint8_t *)&flip)[i] = 0;
    flip.crtc_id = 1;  /* DRM_CRTC_ID */
    flip.fb_id = drm_fb_id;
    flip.flags = 0;    /* fire-and-forget, mirrors old synchronous KMS_FLIP */
    ioctl(display_dev_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
}

#endif
