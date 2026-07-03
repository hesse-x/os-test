/* Minimal DRM test: open /dev/dri/card0, create dumb, map, draw pattern, addfb, setcrtc, page flip */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/poll.h>
#include "syscall.h"
#include "utils/drm.h"
#include "xos/ioctl.h"

#define DRM_FB_WIDTH  800
#define DRM_FB_HEIGHT 600

int main(void) {
    printf("drm_test: starting\n");

    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { printf("drm_test: open failed fd=%d\n", fd); return 1; }
    printf("drm_test: opened fd=%d\n", fd);

    /* SET_MASTER */
    int rc = ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
    printf("drm_test: SET_MASTER rc=%d\n", rc);

    /* GET_CAP */
    struct drm_get_cap cap = { .capability = DRM_CAP_DUMB_BUFFER };
    rc = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
    printf("drm_test: GET_CAP DUMB_BUFFER rc=%d value=%llu\n", rc, (unsigned long long)cap.value);

    /* GETRESOURCES */
    struct drm_mode_card_res res;
    __builtin_memset(&res, 0, sizeof(res));
    rc = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    printf("drm_test: GETRESOURCES rc=%d crtcs=%u connectors=%u encoders=%u\n",
           rc, res.count_crtcs, res.count_connectors, res.count_encoders);

    /* CREATE_DUMB */
    struct drm_mode_create_dumb dumb = {
        .width = DRM_FB_WIDTH, .height = DRM_FB_HEIGHT, .bpp = 32
    };
    rc = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
    printf("drm_test: CREATE_DUMB rc=%d handle=%u pitch=%u size=%llu\n",
           rc, dumb.handle, dumb.pitch, (unsigned long long)dumb.size);
    if (rc < 0) { printf("drm_test: CREATE_DUMB failed, abort\n"); return 1; }

    /* MAP_DUMB + mmap */
    struct drm_mode_map_dumb map = { .handle = dumb.handle };
    rc = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    printf("drm_test: MAP_DUMB rc=%d offset=%llu\n", rc, (unsigned long long)map.offset);

    uint8_t *buf = (uint8_t *)mmap(NULL, dumb.size, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, map.offset);
    if (buf == MAP_FAILED) { printf("drm_test: mmap failed\n"); return 1; }
    printf("drm_test: mmap'd at %p\n", buf);

    /* Draw test pattern: solid fill + horizontal/vertical lines + rectangle */
    for (int y = 0; y < DRM_FB_HEIGHT; y++) {
        for (int x = 0; x < DRM_FB_WIDTH; x++) {
            uint32_t color = 0x0000A0;  /* dark blue background */
            if (x < 4 || x >= DRM_FB_WIDTH - 4) color = 0xFFFFFF;  /* vertical white lines */
            if (y < 4 || y >= DRM_FB_HEIGHT - 4) color = 0xFFFFFF;  /* horizontal white lines */
            if (x >= 100 && x < 200 && y >= 100 && y < 200) color = 0xFF0000;  /* red rect */
            if (x >= 300 && x < 400 && y >= 100 && y < 200) color = 0x00FF00;  /* green rect */
            if (x >= 500 && x < 600 && y >= 100 && y < 200) color = 0x0000FF;  /* blue rect */
            uint32_t *px = (uint32_t *)(buf + y * dumb.pitch + x * 4);
            *px = color;
        }
    }
    printf("drm_test: pattern drawn\n");

    /* ADDFB */
    struct drm_mode_fb_cmd fb = {
        .width = DRM_FB_WIDTH, .height = DRM_FB_HEIGHT,
        .pitch = dumb.pitch, .bpp = 32, .depth = 24, .handle = dumb.handle
    };
    rc = ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb);
    printf("drm_test: ADDFB rc=%d fb_id=%u\n", rc, fb.fb_id);

    /* SETCRTC */
    struct drm_mode_crtc crtc;
    __builtin_memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = 1;  /* DRM_CRTC_ID */
    crtc.fb_id = fb.fb_id;
    crtc.mode_valid = 1;
    crtc.mode.hdisplay = DRM_FB_WIDTH; crtc.mode.vdisplay = DRM_FB_HEIGHT;
    crtc.mode.vrefresh = 60;
    rc = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
    printf("drm_test: SETCRTC rc=%d\n", rc);

    /* PAGE_FLIP */
    struct drm_mode_crtc_page_flip flip = {
        .crtc_id = 1, .fb_id = fb.fb_id, .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = 42
    };
    rc = ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
    printf("drm_test: PAGE_FLIP rc=%d\n", rc);

    /* poll + read event */
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int prc = poll(&pfd, 1, 1000);
    printf("drm_test: poll rc=%d revents=0x%x\n", prc, pfd.revents);
    if (prc > 0 && (pfd.revents & POLLIN)) {
        struct drm_event_vblank ev;
        int rrc = read(fd, &ev, sizeof(ev));
        printf("drm_test: read event rc=%d type=%u user_data=%llu\n",
               rrc, ev.type, (unsigned long long)ev.user_data);
    }

    printf("drm_test: done — check screen for test pattern\n");
    while (1) { struct recv_msg m; recv(&m, NULL, 0, 0); }
    return 0;
}
