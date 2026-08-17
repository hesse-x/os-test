/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
// Parked stdlib subset that musl upstream cannot yet replace in this OS
// (stdlib.md). Everything else (abs/labs/llabs/imaxabs/imaxdiv/div/ldiv/lldiv,
// atoi/atol/atoll, strtol/strtoul/strtoll/strtoull/strtoimax/strtoumax,
// strtod/strtof/strtold/atof, qsort/bsearch, rand/srand/rand_r, exit/atexit/
// abort/quick_exit/at_quick_exit/_Exit, environ/getenv/setenv/putenv/unsetenv/
// clearenv, __libc_start_main + the .init/.fini-array + env startup chain) now
// comes from musl upstream via musl_stdlib_objs.
//
// Remaining here:
//   mknod/chmod — wrap sys_mknod / sys_chmod (kernel has these syscalls;
//                 musl's wrappers route through the same numbers, but kept
//                 here to avoid pulling src/misc/sysm.c machinery).
//
// Moved OUT to musl:
//   stdio.md   — remove/getline/getdelim/fscanf/scanf/sscanf/vfscanf (were
//                ENOSYS stubs; musl src/stdio now supplies them).
//   stdlib.md  — mkstemp/mktemp/mkostemp (src/temp), realpath (src/misc).
//   this batch — getpagesize (src/legacy/getpagesize.c) + sysconf
//                (src/conf/sysconf.c), backed by the now-implemented
// SYS_sysinfo / SYS_prlimit64 / SYS_sched_getaffinity. The OS-private
//                sys_sysconf syscall stays (test_mprotect.c calls it directly).

#include <errno.h> // IWYU pragma: keep
#include <stdint.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <xos/errno.h>
#include <xos/syscall_ext.h>

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

// getpagesize / sysconf — ADOPTED musl upstream (src/legacy/getpagesize.c +
// src/conf/sysconf.c). musl sysconf's dynamic branches are backed by the
// now-implemented SYS_sysinfo / SYS_prlimit64 / SYS_sched_getaffinity; the
// repo's sys_sysconf-backed sysconf and the trivial getpagesize are deleted
// here. The LIBC_EXPORT re-declarations in <xos/unistd_ext.h> stay (visibility
// under -fvisibility=hidden). The OS-private sys_sysconf syscall remains for
// test_mprotect.c, which calls it directly.
