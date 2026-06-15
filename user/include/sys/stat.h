#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stat {
    off_t st_size;
};

int stat(const char *path, struct stat *st);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STAT_H */
