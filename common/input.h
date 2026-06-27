#ifndef COMMON_INPUT_H
#define COMMON_INPUT_H

#include <stdint.h>

// ioctl arg for KBD_IOCTL_BIND
struct kbd_ioctl_bind_arg {
    uint32_t pid;    // consumer PID
    int32_t  result; // output: 0 on success
};

#endif /* COMMON_INPUT_H */
