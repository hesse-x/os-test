#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_NONBLOCK  4

#define F_GETFL     1
#define F_SETFL     2

#ifdef __cplusplus
extern "C" {
#endif

int open(const char *path, int flags, ...);
int dup2(int old_fd, int new_fd);
int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif /* _FCNTL_H */
