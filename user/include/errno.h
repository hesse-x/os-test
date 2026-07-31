/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin shim that forwards to the unmodified musl <errno.h>. musl's header is
 * the file-level replacement for this OS's legacy hand-written <errno.h>
 * (musl_worklist errno module); it is NOT copied into the repo — the compiler
 * resolves "musl/include/errno.h" via the -I third_party include path. musl's
 * <errno.h> #includes <bits/errno.h> (the E*-macro table — there is no
 * user/include/bits/errno.h, so it resolves to musl's arch/generic copy, the
 * same table include/uapi/xos/errno.h mirrors 1:1), declares __errno_location,
 * and defines the errno macro. Under _GNU_SOURCE it also declares
 * program_invocation_short_name/name — both ARE defined (musl libc.c
 * weak_alias to __progname/__progname_full, set by __libc_start_main), so the
 * declaration carries no undefined-reference risk.
 *
 * __errno_location itself is provided by musl src/errno/__errno_location.c
 * (compiled in musl_pthread) and exported via libc.map; the hand-written
 * LIBC_EXPORT decl that used to live here is dropped because musl's header
 * declares it without a visibility attribute (the version script gates the
 * export, as for every other musl symbol).
 *
 * The kernel keeps its own include/uapi/xos/errno.h as the frozen UAPI source
 * (numerically identical to musl's bits/errno.h); no errno_sync.c is needed
 * because there is no second live userspace header adding OS-specific errnos.
 * install-headers.sh publishes musl's real <errno.h> + <bits/errno.h> verbatim
 * to the sysroot (replacing this shim, which only resolves with -I third_party
 * at build time).
 */
#ifndef _USER_ERRNO_SHIM_H
#define _USER_ERRNO_SHIM_H

#include "musl/include/errno.h"

#endif
