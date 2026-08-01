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
 * (time module not yet migrated); getpid-based impl kept. realpath        —
 * musl src/misc/realpath.c needs /proc/self/fd/N + readlink
 *                     + O_PATH (no procfs); getcwd + lexical collapse kept.
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
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/cdefs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xos/errno.h>
#include <xos/unistd_ext.h>

// ==================== mkstemp / mktemp (group 3) ====================
// Replace the trailing X's in template with random letters, then open with
// O_CREAT|O_EXCL|O_RDWR so a pre-existing name yields EEXIST and we retry.
// Relies on the O_EXCL semantics enforced by sys_open (via i_op->create).
static int fill_xxx(char *tmpl, int xstart, int xlen) {
  // Seed from getpid + a monotonic counter so concurrent/sequential calls in
  // one process produce distinct names without requiring srand().
  static unsigned counter = 0;
  unsigned seed = (unsigned)getpid() * 2654435761u + (counter++ * 2246822519u);
  const char *set = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123"
                    "456789";
  for (int i = 0; i < xlen; i++) {
    seed = seed * 1103515245u + 12345u;
    tmpl[xstart + i] = set[(seed / 65536) % 62];
  }
  return 0;
}

// Find the run of trailing X's in template (POSIX requires >=6). Returns the
// start index and length via out params, or -1 if none.
static int find_xrun(char *tmpl, int *start, int *len) {
  int slen = (int)strlen(tmpl);
  int i = slen;
  while (i > 0 && tmpl[i - 1] == 'X')
    i--;
  if (i == slen)
    return -1;
  *start = i;
  *len = slen - i;
  return 0;
}

LIBC_EXPORT int mkstemp(char *tmpl) {
  int start, len;
  if (find_xrun(tmpl, &start, &len) < 0 || len < 6) {
    errno = EINVAL;
    return -1;
  }
  // Try up to 2^len distinct names (capped).
  for (int attempt = 0; attempt < 256; attempt++) {
    fill_xxx(tmpl, start, len);
    int fd = open(tmpl, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0)
      return fd;
    if (errno != EEXIST)
      return -1;
  }
  errno = EEXIST;
  return -1;
}

// mktemp: fill the template with a unique name that does not exist, without
// opening it. Returns template on success, "" on failure. Inherently racy
// (POSIX warns so); acceptable for this libc.
LIBC_EXPORT char *mktemp(char *tmpl) {
  int start, len;
  if (find_xrun(tmpl, &start, &len) < 0 || len < 6) {
    tmpl[0] = '\0';
    return tmpl;
  }
  for (int attempt = 0; attempt < 256; attempt++) {
    fill_xxx(tmpl, start, len);
    struct stat st;
    if (stat(tmpl, &st) < 0) {
      if (errno == ENOENT)
        return tmpl; // name is free
      tmpl[0] = '\0';
      return tmpl;
    }
  }
  tmpl[0] = '\0';
  return tmpl;
}

// ==================== realpath (group 3) ====================
// No symlinks exist in this FS yet, so realpath reduces to: make the path
// absolute (relative → getcwd join) then collapse . / .. / redundant slashes.
// When resolved is NULL, POSIX requires a caller-owned allocated result.
LIBC_EXPORT char *realpath(const char *path, char *resolved) {
  if (!path || !path[0]) {
    errno = EINVAL;
    return NULL;
  }
  char *buf = resolved;
  if (!buf) {
    buf = malloc(4096);
    if (!buf) {
      errno = ENOMEM;
      return NULL;
    }
  }

  char abs[4096];
  if (path[0] == '/') {
    strncpy(abs, path, sizeof(abs) - 1);
    abs[sizeof(abs) - 1] = '\0';
  } else {
    if (!getcwd(abs, sizeof(abs))) {
      goto fail;
    }
    size_t cl = strlen(abs);
    if (cl + 1 + strlen(path) + 1 > sizeof(abs)) {
      errno = ENAMETOOLONG;
      goto fail;
    }
    abs[cl++] = '/';
    strcpy(abs + cl, path);
  }

  // Canonicalize: split on '/', drop empty + ".", apply "..".
  size_t outcap = 4096;
  // Stack of component start offsets within buf.
  int starts[256];
  int depth = 0;
  size_t o = 0;
  const char *p = abs;
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break;
    const char *seg = p;
    while (*p && *p != '/')
      p++;
    size_t seglen = (size_t)(p - seg);
    if (seglen == 1 && seg[0] == '.')
      continue;
    if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
      if (depth > 0) {
        depth--;
        // Pop the previous component: starts[depth] points just past its
        // leading '/', so rewind one further to drop that '/' too. This
        // prevents the next component from producing a doubled "//".
        o = (size_t)starts[depth] - 1;
      }
      continue;
    }
    if (depth == (int)(sizeof(starts) / sizeof(starts[0])) || o + 1 >= outcap) {
      errno = ENAMETOOLONG;
      goto fail;
    }
    buf[o++] = '/';
    starts[depth++] = (int)o;
    if (o + seglen >= outcap) {
      errno = ENAMETOOLONG;
      goto fail;
    }
    memcpy(buf + o, seg, seglen);
    o += seglen;
  }
  if (o == 0) {
    buf[o++] = '/';
  }
  buf[o] = '\0';
  return buf;

fail:
  if (!resolved)
    free(buf);
  return NULL;
}

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
