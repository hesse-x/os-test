#ifndef _FCNTL_H
#define _FCNTL_H

#include <stdint.h>

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_NONBLOCK  4
#define O_APPEND    8
#define O_CREAT    16
#define O_TRUNC    32

#define F_GETFL     1
#define F_SETFL     2

// Sealing constants (for memfd_create + fcntl)
#define F_ADD_SEALS   1033
#define F_GET_SEALS   1034
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#define F_SEAL_WRITE  0x0008

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
