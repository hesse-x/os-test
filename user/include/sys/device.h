#ifndef _SYS_DEVICE_H
#define _SYS_DEVICE_H

#include <sys/types.h>
#include "common/dev.h"

#ifdef __cplusplus
extern "C" {
#endif

int device_register(const char *name, int dev_type);
int device_register_shm(const char *name, int dev_type, int shm_fd);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_DEVICE_H */
