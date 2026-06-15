// KMS driver process (user-space)
// Creates display SHM (back buffer), does memcpy flip (back → front).
// IOPL=0, no IRQ binding. Framebuffer mapped via mmap MAP_PHYSICAL.
#include <stdint.h>
#include <stddef.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/device.h>
#include <sys/fb.h>
#include "common/shm.h"
#include "common/dev.h"
#include "common/macro.h"
#include "driver/display.h"

// ===================== Main loop =====================

extern "C" void _start() {
    // Initialize display backend: get fb info → create display SHM →
    // write header → register DEV_KMS
    if (display_backend_init() < 0) {
        // No framebuffer, just idle
        while (1) { struct recv_msg msg; recv(&msg, NULL, 0, 0); }
    }

    // Flip loop: poll for dirty → memcpy back→front → wait if idle
    while (1) {
        if (display_backend_poll()) {
            continue;  // Still dirty, process again
        }
        display_backend_wait(16);  // No dirty, wait 16ms or notify
    }
}
