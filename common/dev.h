#ifndef COMMON_DEV_H
#define COMMON_DEV_H

// Device type enum for sys_load_dev / sys_lookup_dev
#define DEV_TYPE_MAX  32

#define DEV_NONE      0   // slot empty / not found
#define DEV_DISK      1
#define DEV_KBD       2
#define DEV_KMS       3
#define DEV_FS        4
#define DEV_TERMINAL  5

#endif // COMMON_DEV_H
