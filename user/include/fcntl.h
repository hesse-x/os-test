/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin shim that forwards to the unmodified musl <fcntl.h>. musl's header is
 * the file-level replacement for this OS's legacy hand-written <fcntl.h>
 * (fcntl_worklist §3a); it is NOT copied into the repo — the compiler resolves
 * "musl/include/fcntl.h" via the -I third_party include path. musl's <fcntl.h>
 * pulls <bits/fcntl.h> (the byte-identical copy at user/include/bits/fcntl.h),
 * declares open/openat/fcntl/creat/posix_fadvise/posix_fallocate + struct
 * flock + the AT_*, F_*, F_SEAL_*, O_* constants. FD_CLOEXEC, F_OK/R_OK/W_OK/
 * X_OK and SEEK_* are all provided by musl's header. musl's openat is adopted
 * (the kernel's sys_openat resolves AT_FDCWD to bp->cwd via the M0.4
 * resolve_dirfd_start fix), so no OS-specific openat re-declaration is needed.
 *
 * The kernel no longer shares a UAPI fcntl header — it uses its private
 * kernel/bsd/kfcntl.h; cross-consistency is locked at compile time by
 * kernel/bsd/fcntl_sync.c (fcntl_worklist §3c). install-headers.sh publishes
 * musl's real <fcntl.h> verbatim to the sysroot (replacing this shim, which
 * only resolves with -I third_party at build time).
 */
#ifndef _USER_FCNTL_SHIM_H
#define _USER_FCNTL_SHIM_H

#include "musl/include/fcntl.h"

#endif
