#ifndef COMMON_IOCTL_H
#define COMMON_IOCTL_H

#include <stdint.h>

// ioctl command encoding (Linux-compatible)
// Shared between kernel and user space.
#define _IOC(dir, type, nr, size) \
    ((uint32_t)(((dir) << 30) | ((type) << 8) | ((nr) << 0) | ((size) << 16)))
#define _IO(type, nr)       _IOC(0, type, nr, 0)
#define _IOW(type, nr, sz)  _IOC(1, type, nr, sizeof(sz))
#define _IOR(type, nr, sz)  _IOC(2, type, nr, sizeof(sz))
#define _IOWR(type, nr, sz) _IOC(3, type, nr, sizeof(sz))
#define _IOC_DIR(cmd)   (((cmd) >> 30) & 3)
#define _IOC_TYPE(cmd)  (((cmd) >> 8) & 0xFF)
#define _IOC_NR(cmd)    ((cmd) & 0xFF)
#define _IOC_SIZE(cmd)  (((cmd) >> 16) & 0x3FFF)
#define _IOC_NONE   0
#define _IOC_WRITE  1
#define _IOC_READ   2

// ===== ioctl command definitions (_IOC-encoded) =====

// Terminal (Linux-compatible, already encoded)
#define TCGETS  0x5401

// KMS display
#define KMS_IOCTL_CREATE_BUF  _IOWR('K', 1, char[32])   // unified struct = 32B
#define KMS_IOCTL_FLIP        _IO('K', 2)

// Keyboard
#define KBD_IOCTL_BIND        _IOWR('K', 0x11, char[8])  // kbd_ioctl_bind_arg = 8B
#define KBD_IOCTL_UNBIND      _IO('K', 0x12)

#endif
