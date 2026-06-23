#ifndef KERNEL_DISPLAY_H
#define KERNEL_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/sparse.h"

struct proc_t;

#define DISPLAY_FONT_WIDTH   8
#define DISPLAY_FONT_HEIGHT  16

// Display request constants
#define DISPLAY_REQ_CREATE_BUF  1
#define DISPLAY_REQ_FLIP        2

// Request/Response structures
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

// Kernel display subsystem state
struct display_state {
    uint8_t __iomem *front_fb;      // front buffer MMIO address
    uint8_t  *back_buffer;          // back buffer kernel virtual address
    uint64_t  back_buffer_phys;  // back buffer physical address
    uint64_t  back_buffer_npages;// back buffer page count
    uint32_t  fb_width;
    uint32_t  fb_height;
    uint32_t  fb_pitch;
    uint32_t  fb_bpp;
    uint32_t  fb_size;
    bool      initialized;       // back buffer allocated
};

// Global display state (defined in display.c)
extern struct display_state g_display;

// Request handler
int display_req_handler(uint32_t req_type, void *req_data, uint32_t req_len,
                        void *resp_data, uint32_t resp_len);

// mmap handler: returns mapped address, 0=failure
uint64_t display_mmap_handler(struct proc_t *proc, size_t size);

// Device registration (called after vfs_init)
void display_dev_register(void);

#endif
