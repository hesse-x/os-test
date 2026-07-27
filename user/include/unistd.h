/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin shim that forwards to the unmodified musl <unistd.h>. musl's header is
 * the file-level replacement for this OS's legacy <unistd.h> (plan §4.5); it is
 * NOT copied into the repo — the compiler resolves "musl/include/unistd.h" via
 * the -I third_party include path. OS-specific declarations that musl's
 * <unistd.h> does not carry live in <xos/unistd_ext.h> (gettid,
 * wait_dev_ready), <sys/io.h> (ioperm), and <sys/stat.h> (umask, utimensat).
 */
#ifndef _USER_UNISTD_SHIM_H
#define _USER_UNISTD_SHIM_H

/* musl's <unistd.h> unconditionally #defines NULL (lines 18-22). This collides
 * with the NULL already defined by the freestanding <stddef.h> we ship
 * (clang's __stddef_null.h uses __null), tripping -Wmacro-redefined under
 * -Werror when <stdio.h>/<stddef.h> is included before <unistd.h>. Drop the
 * prior definition so musl's takes effect cleanly, then re-include <stddef.h>
 * afterward so downstream consumers see the toolchain's NULL again. */
#ifdef NULL
#undef NULL
#endif
#include "musl/include/unistd.h"
#include <stddef.h>

#endif
