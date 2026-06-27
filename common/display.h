#ifndef COMMON_DISPLAY_H
#define COMMON_DISPLAY_H

#include <stdint.h>

// Legacy request constants (still used by display_req_handler)
#define DISPLAY_REQ_CREATE_BUF  1
#define DISPLAY_REQ_FLIP        2

// Unified ioctl arg for KMS_IOCTL_CREATE_BUF (_IOWR, 32 bytes)
struct display_ioctl_create_buf_arg {
    // input
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    // output (filled by kernel)
    uint32_t pitch;
    uint32_t size;
    uint32_t rows;
    uint32_t cols;
    int32_t  result;
};

// Request/Response structures (legacy, used by display_req_handler)
struct display_create_buf_req {
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

#endif /* COMMON_DISPLAY_H */
