#ifndef _SYS_DEVICE_H
#define _SYS_DEVICE_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int device_register(const char *name, int dev_type);

// Device type constants (must match common/dev.h)
#define DEV_SERIAL   6

#ifdef __cplusplus
}
#endif

#endif /* _SYS_DEVICE_H */
