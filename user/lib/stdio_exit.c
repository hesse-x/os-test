/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * __stdio_exit — strong override of musl exit.c's `weak_alias(dummy,
 * __stdio_exit)`.
 *
 * musl's exit() calls __stdio_exit() after the atexit/fini chain to flush every
 * stdio FILE. musl's own __stdio_exit (src/stdio/__stdio_exit.c) walks its
 * internal ofl linked list + the __stdin_used/__stdout_used/__stderr_used
 * dummies — but this repo keeps a hand-written stdio (user/lib/stdio.cc) whose
 * FILE objects are NOT registered on musl's ofl list, so musl's version would
 * flush nothing. This strong definition replaces it: flush stdout + stderr
 * (which covers the kernel console + serial fd backing them). It is reached
 * via exit() → __stdio_exit on every process exit, so printf output without a
 * trailing newline still flushes before _Exit.
 *
 * Compiled under the repo libc headers (LIBC_SOURCES), NOT add_musl_lib: the
 * FILE/fflush/stdout/stderr it touches are the repo's own (user/include/stdio.h
 * + user/lib/stdio.cc), not musl's internal FILE. C linkage means the symbol
 * name is all that matters, so musl exit.o's weak call resolves here.
 */

#include <stdio.h>

void __stdio_exit(void) {
  fflush(stdout);
  fflush(stderr);
}
