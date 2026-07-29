/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COMMON_DIRENT_H
#define _COMMON_DIRENT_H

#include <stdint.h>

/*
 * Kernel-internal dirent64 layout — used by sys_getdents (syscall 217 and the
 * aliased 78) and by libc's readdir(). Both kernel (fat32.c) and the musl
 * dirent sources (third_party/musl/src/dirent) consume this same layout, so it
 * must stay field-for-field identical to musl's struct dirent (and to
 * user/include/dirent.h's struct dirent).
 *
 * musl's readdir() returns a pointer straight into the getdents buffer cast to
 * struct dirent — no per-field copy — so the two structs are intentionally
 * layout-identical, not unrelated. The kernel fills d_type (DT_DIR/DT_REG/...).
 */
struct dirent64 {
  uint64_t d_ino;
  uint64_t d_off; /* offset for seekdir/telldir (fs-specific: fat32=entry index,
                     in-memory=byte offset) */
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[];
};

#endif
