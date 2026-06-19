#ifndef _SYS_DEVICE_H
#define _SYS_DEVICE_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int device_register(pid_t pid, int dev_type);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_DEVICE_H */
