/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin shim that forwards to the unmodified musl <stdio.h>. musl's header is
 * the file-level replacement for this OS's legacy hand-written <stdio.h>
 * (musl_worklist stdio module); it is NOT copied into the repo — the compiler
 * resolves "musl/include/stdio.h" via the -I third_party include path.
 *
 * musl's <stdio.h> pulls <bits/alltypes.h> (FILE = struct _IO_FILE,
 * size_t/off_t/ssize_t/va_list) and declares the full stdio family: the
 * printf/scanf engines (vfprintf/vfscanf) + wrappers, the byte getc/putc
 * family, fopen/fclose/fread/fwrite/fseek/ftell, open_memstream/fmemopen,
 * getdelim/getline, flockfile/..., and the *_unlocked variants. The actual
 * implementations come from musl upstream src/stdio (musl_stdio_objs,
 * under third_party/musl/src/stdio). FILE is musl's struct _IO_FILE
 * (src/internal/stdio_impl.h) — NOT this OS's retired hand-written struct
 * _FILE; bits/alltypes.h already typedefs FILE = struct _IO_FILE, so the type
 * alias is consistent.
 *
 * This OS has NO declaration that musl's <stdio.h> lacks (unlike the mman
 * module's memfd_create), so the shim is a pure forwarder with no appended
 * declarations. musl gates a subset (getline/asprintf/fdopen/fileno/flockfile/
 * fseeko/open_memstream/renameat/dprintf/the _unlocked variants/...) behind
 * _POSIX_C_SOURCE
 * /_GNU_SOURCE/_BSD_SOURCE; a tree-wide scan found no repo consumer calls a
 * gated stdio symbol without first defining the matching feature macro
 * (shell.cc uses only printf/putchar, which are ungated), so this switch
 * breaks no existing build.
 *
 * install-headers.sh publishes musl's real <stdio.h> verbatim to the sysroot
 * (replacing this shim, which only resolves with -I third_party at build time).
 */
#ifndef _USER_STDIO_SHIM_H
#define _USER_STDIO_SHIM_H

#include "musl/include/stdio.h"

#endif /* _USER_STDIO_SHIM_H */
