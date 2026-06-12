#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stddef.h>
#include "common/shm.h"

extern "C" {

struct boot_info;

// Global framebuffer info (filled by init_fb, read by shm_init for KMS)
extern kms_fb_info g_fb_info;

// Initialize framebuffer mapping (called from init_mem)
void init_fb(boot_info *bi);

}

#endif // FB_H
