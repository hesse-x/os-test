/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin shim that forwards to the unmodified musl <sys/mman.h>. musl's header
 * is the file-level replacement for this OS's legacy hand-written <sys/mman.h>
 * (musl_worklist mman module); it is NOT copied into the repo — the compiler
 * resolves "musl/include/sys/mman.h" via the -I third_party include path.
 * musl's <sys/mman.h> pulls <bits/alltypes.h> (mode_t/size_t/off_t) +
 * <bits/mman.h> (the repo's user/include/bits/mman.h, which supplies MAP_32BIT
 * + the OS-specific
 * MAP_FIXED_NOREPLACE/MAP_SHARED_VALIDATE/MAP_GROWSUP/PROT_SEM/ MFD_xx
 * constants musl's generic header lacks, with static_assert parity against
 * include/uapi/xos/mman.h). It declares mmap/munmap/mprotect/msync/
 * mremap/mlock/munlock/mlockall/munlockall/posix_madvise/madvise/mincore/
 * shm_open/shm_unlink/remap_file_pages — all now implemented by musl upstream
 * (musl_mman_objs, under third_party/musl/src/mman).
 *
 * The ONE declaration musl's <sys/mman.h> does NOT carry that this OS exposes
 * is memfd_create (musl has no memfd_create in src/mman — it lives in musl
 * src/linux/, which is not adopted here). The hand-written memfd_create
 * wrapper is retained in user/lib/sys_process.cc (routes to
 * SYS_MEMFD_CREATE=319); its declaration is re-added below with LIBC_EXPORT so
 * consumers of <sys/mman.h> see it, matching glibc's placement of memfd_create
 * under <sys/mman.h> (_GNU_SOURCE).
 *
 * install-headers.sh publishes musl's real <sys/mman.h> verbatim to the
 * sysroot (replacing this shim, which only resolves with -I third_party at
 * build time) and publishes user/include/bits/mman.h to bits/mman.h; the
 * memfd_create declaration below is appended to the published sys/mman.h by
 * install-headers.sh's mman step.
 */
#ifndef _USER_SYS_MMAN_SHIM_H
#define _USER_SYS_MMAN_SHIM_H

#include "musl/include/sys/mman.h"

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* memfd_create: create an anonymous file backed by memory. OS-specific
 * (musl src/mman has no memfd_create); defined in user/lib/sys_process.cc,
 * routes to SYS_MEMFD_CREATE (319). Flags MFD_CLOEXEC/MFD_ALLOW_SEALING come
 * from <bits/mman.h> (included by musl's <sys/mman.h> above). */
LIBC_EXPORT int memfd_create(const char *name, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* _USER_SYS_MMAN_SHIM_H */
