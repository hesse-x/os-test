#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/display.h"

typedef struct boot_info boot_info;

// Initialize framebuffer mapping (called from init_mem)
void init_fb(boot_info *bi);

#endif // FB_H
