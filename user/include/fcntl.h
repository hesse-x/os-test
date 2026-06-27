#ifndef _FCNTL_H
#define _FCNTL_H

#include "common/fcntl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int open(const char *path, int flags, ...);
int dup2(int old_fd, int new_fd);
int fcntl(int fd, int cmd, ...);
uint64_t fd_file_size(int fd);

#ifdef __cplusplus
}
#endif

#endif /* _FCNTL_H */
