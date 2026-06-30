#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stdint.h>

void    vfs_init(void);

int64_t sys_open(int64_t arg1, int64_t arg2, int64_t arg3,
                  int64_t _u1, int64_t _u2, int64_t _u3);
int64_t sys_stat(int64_t arg1, int64_t arg2, int64_t _u1,
                  int64_t _u2, int64_t _u3, int64_t _u4);
int64_t sys_mkdir(int64_t arg1, int64_t arg2, int64_t _u1,
                   int64_t _u2, int64_t _u3, int64_t _u4);
int64_t sys_unlink(int64_t arg1, int64_t _u1, int64_t _u2,
                    int64_t _u3, int64_t _u4, int64_t _u5);
int64_t sys_rmdir(int64_t arg1, int64_t _u1, int64_t _u2,
                   int64_t _u3, int64_t _u4, int64_t _u5);
int64_t sys_dev_create(int64_t arg1, int64_t arg2, int64_t _u1,
                        int64_t _u2, int64_t _u3, int64_t _u4);
int64_t sys_getdents(int64_t arg1, int64_t arg2, int64_t arg3,
                      int64_t _u1, int64_t _u2, int64_t _u3);

#endif
