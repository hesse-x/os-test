#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAME_MAX 255

struct dirent {
    ino_t d_ino;
    char  d_name[NAME_MAX + 1];
};

typedef struct {
    int dd_fd;
    // Internal state managed by libc
} DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#ifdef __cplusplus
}
#endif

#endif /* _DIRENT_H */
