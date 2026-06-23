#ifndef _COMMON_DIRENT_H
#define _COMMON_DIRENT_H

#include <stdint.h>

/*
 * Kernel-internal dirent64 layout — used by sys_getdents and libc readdir().
 * Both kernel (fat32.c) and userspace (user/lib/file.cc) must use this
 * single definition so the layout can never silently diverge.
 */
struct dirent64 {
    uint64_t d_ino;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

#endif
