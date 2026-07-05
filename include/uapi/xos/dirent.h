#ifndef _COMMON_DIRENT_H
#define _COMMON_DIRENT_H

#include <stdint.h>

/*
 * Kernel-internal dirent64 layout — used by sys_getdents and libc readdir().
 * Both kernel (fat32.c) and userspace (user/lib/file.cc) must use this
 * single definition so the layout can never silently diverge.
 *
 * NOTE: this is NOT a duplicate of user/include/dirent.h's `struct dirent`.
 * The two are deliberately different and intentionally unrelated by field:
 *   - struct dirent64  (here): variable-length, d_reclen/d_type, what the
 *     kernel hands back from sys_getdents into a caller-provided buffer.
 *   - struct dirent    (user/include/dirent.h): fixed NAME_MAX+1 name, what
 *     libc's readdir() returns to applications. libc's readdir() reads
 *     dirent64 records via sys_getdents and copies d_name into a dirent.
 */
struct dirent64 {
  uint64_t d_ino;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[];
};

#endif
