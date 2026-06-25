#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stdint.h>

void    vfs_init(void);

uint64_t sys_open(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                  uint64_t _u1, uint64_t _u2, uint64_t _u3);
uint64_t sys_stat(uint64_t arg1, uint64_t arg2, uint64_t _u1,
                  uint64_t _u2, uint64_t _u3, uint64_t _u4);
uint64_t sys_mkdir(uint64_t arg1, uint64_t arg2, uint64_t _u1,
                   uint64_t _u2, uint64_t _u3, uint64_t _u4);
uint64_t sys_unlink(uint64_t arg1, uint64_t _u1, uint64_t _u2,
                    uint64_t _u3, uint64_t _u4, uint64_t _u5);
uint64_t sys_rmdir(uint64_t arg1, uint64_t _u1, uint64_t _u2,
                   uint64_t _u3, uint64_t _u4, uint64_t _u5);
uint64_t sys_dev_create(uint64_t arg1, uint64_t arg2, uint64_t _u1,
                        uint64_t _u2, uint64_t _u3, uint64_t _u4);
uint64_t sys_getdents(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                      uint64_t _u1, uint64_t _u2, uint64_t _u3);

#endif
