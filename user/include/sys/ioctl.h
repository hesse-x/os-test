#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <stdint.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ioctl_cmd_t;

LIBC_EXPORT int ioctl(int fd, ioctl_cmd_t cmd, ...);

#include <xos/ioctl.h>

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IOCTL_H */
