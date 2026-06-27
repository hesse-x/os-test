#ifndef COMMON_DEV_H
#define COMMON_DEV_H

// Device type enum
#define DEV_TYPE_MAX  32

#define DEV_NONE      0   // slot empty / not found
#define DEV_KBD       2
#define DEV_KMS       3
#define DEV_BLOCK     4   /* 块设备（/dev/sda 等）*/
#define DEV_TERMINAL  5
#define DEV_SERIAL    6
#define DEV_PTMX      7
#define DEV_PTS_SLAVE 8

#endif // COMMON_DEV_H
