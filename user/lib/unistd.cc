/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libc unistd residual wrappers.
//
// Most of the legacy <unistd.h> surface is now supplied by the upstream musl
// sources built as musl_unistd_objs (see user/CMakeLists.txt, unistd_try.md):
// read/write/close/dup/dup2/lseek/pipe/pipe2/fsync/ftruncate/getpid/get*id/
// set*id/alarm/pause/sleep/usleep/sync/truncate/chdir/getcwd/rename/unlink/
// mkdir/rmdir/access/symlink/link/readlink/... and the cancellable/set*id
// helpers via lib/musl_shim/syscall_cp.c. Only the two OS-specific entry
// points that musl's src/unistd does NOT provide remain here:
//   ioperm — port I/O permission bitmap (musl declares it in <sys/io.h>
//            but has no src/unistd/ioperm.c; this is the IOPM syscall).
//   umask  — process file-creation mask (musl implements it in src/stat/,
//            not src/unistd/, so it is not pulled in by the unistd glob).
#include "sys/cdefs.h"
#include <syscall.h>

#include <sys/io.h>
#include <sys/stat.h>
#include <sys/types.h>

LIBC_EXPORT int ioperm(unsigned long from, unsigned long num, int turn_on) {
  return sys_ioperm(from, num, turn_on);
}

mode_t umask(mode_t mask) { return (mode_t)sys_umask((int)mask); }
