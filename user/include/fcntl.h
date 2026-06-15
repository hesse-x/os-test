#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2

#ifdef __cplusplus
extern "C" {
#endif

int open(const char *path, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif /* _FCNTL_H */
