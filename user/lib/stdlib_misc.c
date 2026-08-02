/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Parked stdlib subset that musl upstream cannot yet replace in this OS
 * (stdlib.md). Everything else (abs/labs/llabs/imaxabs/imaxdiv/div/ldiv/lldiv,
 * atoi/atol/atoll, strtol/strtoul/strtoll/strtoull/strtoimax/strtoumax,
 * strtod/strtof/strtold/atof, qsort/bsearch, rand/srand/rand_r, exit/atexit/
 * abort/quick_exit/at_quick_exit/_Exit, environ/getenv/setenv/putenv/unsetenv/
 * clearenv, __libc_start_main + the .init/.fini-array + env startup chain) now
 * comes from musl upstream via musl_stdlib_objs.
 *
 * Remaining here:
 *   mkstemp/mktemp  — musl src/temp/ tree needs __randname → __clock_gettime
 * (time module not yet migrated); getpid-based impl kept.
 *   mknod/chmod     — wrap sys_mknod / sys_chmod (kernel has these syscalls;
 *                     musl's wrappers route through the same numbers, but
 *                     kept here to avoid pulling src/misc/sysm.c machinery).
 *   getpagesize/sysconf — musl src/legacy/getpagesize.c + src/conf/sysconf.c
 *                     redefined _SC_NPROCESSORS_ONLN semantics; repo
 *                     sys_sysconf-backed wrappers kept.
 *
 * Moved OUT to musl (stdio.md): remove/getline/getdelim/fscanf/scanf/sscanf/
 * vfscanf — were ENOSYS stubs here (the repo's hand-written stdio.cc had a
 * non-musl FILE layout so musl's scan chain could not link); now supplied by
 * musl src/stdio (musl_stdio_objs). Declares come from: stdlib.h
 * (mkstemp/mktemp/realpath via musl), sys/stat.h (mknod/chmod),
 * xos/unistd_ext.h (getpagesize/sysconf).
 */

#include <errno.h> // IWYU pragma: keep
#include <stdint.h>
#include <unistd.h>
#include <xos/syscall_ext.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <xos/errno.h>
#include <xos/unistd_ext.h>

// ==================== mkstemp / mktemp / mkostemp ====================
// ADOPTED musl upstream (src/temp/{mkstemp,mktemp,mkostemp}.c,
// musl_stdlib_objs): __randname → __clock_gettime dep is satisfied (time module
// migrated), and stdlib.cmake compiles __randname/__mkostemps. The repo's old
// getpid-based mkstemp/mktemp (and fill_xxx/find_xrun helpers) are deleted.
// Declares come from musl's <stdlib.h>.

// ==================== realpath ====================
// ADOPTED musl upstream (src/misc/realpath.c, musl_misc_objs): pure lexical
// resolver (readlink + getcwd + strdup + SYMLOOP_MAX link loop). The old repo
// realpath here assumed "no symlinks exist in this FS" and skipped readlink
// entirely — wrong now that procfs/devtmpfs provide /proc/self and mount
// points. musl's handles symlinks correctly and replaces this. See
// build_script/third_party/musl/modules/misc.cmake.

int mknod(const char *path, mode_t mode, dev_t dev) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  return sys_mknod(path, (uint32_t)mode, (uint64_t)dev);
}

// POSIX functions referenced by upstream libdrm's device-enumeration and
// node-creation paths (chown/chmod/remove/readlink/getline/sscanf/fscanf).
// chown is provided by musl src/unistd (musl_unistd_objs); on x86-64 musl
// routes it to syscall(SYS_chown). chmod below backs the real sys_chmod syscall
// (in-memory only; setuid-bit clearing is in kernel/bsd/syscall.c).
// remove/getline/sscanf/fscanf/scanf used to be ENOSYS stubs here (stdlib_misc
// could not adopt musl's because the repo's hand-written stdio.cc had a
// non-musl FILE layout); the stdio→musl migration (musl_stdio_objs) now
// supplies the real musl remove/getline/getdelim/vfscanf/scanf/fscanf/sscanf,
// so those stubs are deleted from this file.
int chmod(const char *path, mode_t mode) {
  return sys_chmod(path, (unsigned int)mode);
}

// getpagesize / sysconf: musl's <unistd.h> declares getpagesize only under
// _GNU_SOURCE/_BSD_SOURCE (not enabled in the libc build → no prior C
// declaration), and sysconf unconditionally but without a visibility
// attribute (→ HIDDEN under -fvisibility=hidden). The LIBC_EXPORT
// re-declarations in <xos/unistd_ext.h> give these default-visible C-linkage
// definitions. The _SC_* switch constants come from musl's <unistd.h>.
// musl itself places these in src/legacy/getpagesize.c and src/conf/sysconf.c;
// this OS keeps the wrappers here until the musl implementations are wired in
// (M0.2+).
int getpagesize(void) { return 4096; }

long sysconf(int name) {
  // Dynamic values are backed by sys_sysconf (ncpu, total/free phys pages).
  // Static/architecture-fixed values (PAGESIZE, CLK_TCK, OPEN_MAX, …) stay
  // here — glibc hardcodes them too. Unknown → -1, errno unchanged (POSIX).
  switch (name) {
  case _SC_NPROCESSORS_CONF:
  case _SC_NPROCESSORS_ONLN:
  case _SC_PHYS_PAGES:
  case _SC_AVPHYS_PAGES:
    return sys_sysconf(name);
  case _SC_PAGESIZE: // _SC_PAGE_SIZE is the same value (30)
    return 4096;
  case _SC_CLK_TCK:
    return 100;
  case _SC_OPEN_MAX:
    return 128; // MAX_FD (kernel/bsd/types.h)
  default:
    return -1;
  }
}
