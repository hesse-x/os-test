#ifndef _FCNTL_H
#define _FCNTL_H

#include "xos/fcntl.h"
#include <stdint.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX FD_CLOEXEC（用户态约定=1）。内核内部用 0x8000 位，互不干扰。
 * 本 OS 无 exec，F_SETFD/FD_CLOEXEC 仅占位，见 file.cc fcntl 降级处理。 */
#define FD_CLOEXEC 1

LIBC_EXPORT int open(const char *path, int flags, ...);
LIBC_EXPORT int dup2(int old_fd, int new_fd);
LIBC_EXPORT int fcntl(int fd, int cmd, ...);
LIBC_EXPORT uint64_t fd_file_size(int fd);

#ifdef __cplusplus
}
#endif

#endif /* _FCNTL_H */
