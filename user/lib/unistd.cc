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
// helpers via lib/musl_shim/syscall_cp.c. ioperm now comes from musl
// src/linux/ioperm.c (musl_linux_objs, routes to the kernel's SYS_ioperm in
// xcore/trap.c). Only umask remains here:
//   umask — process file-creation mask (musl implements it in src/stat/umask.c,
//           not src/unistd/, so the unistd glob does not pull it in; no module
//           globs src/stat/, so it would otherwise be absent from libc).
#include <syscall.h>

#include <sys/stat.h>
#include <sys/types.h>

mode_t umask(mode_t mask) { return (mode_t)sys_umask((int)mask); }
