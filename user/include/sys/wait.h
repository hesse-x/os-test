/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/cdefs.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

LIBC_EXPORT pid_t waitpid(pid_t pid, int *status, int options);

/* options flags for waitpid() */
#define WNOHANG 1 /* don't block; return 0 if no child exited */
#define WUNTRACED                                                              \
  2 /* also report stopped children (S01: job-control stop state) */
#define WCONTINUED                                                             \
  4 /* also report continued children (not yet implemented)                    \
     */

/* Wait status macros. Encoding matches the kernel's wait status
 * (do_exit: normal exit = (code & 0xff) << 8; death by signal = sig & 0x7f;
 * stopped = (stopsig << 8) | 0x7f). WIFSTOPPED/WSTOPSIG are live as of S01
 * (stopped-state reporting); WIFCONTINUED/WCONTINUED reporting is not yet
 * implemented. */
#define WIFEXITED(status) (!WIFSIGNALED(status) && !WIFSTOPPED(status))
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status)                                                    \
  ((((status) & 0x7f) != 0) && (((status) & 0x7f) < 0x7f))
#define WTERMSIG(status) ((status) & 0x7f)
#define WCOREDUMP(status) ((status) & 0x80) /* S02: core-dump bit */
#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status) (((status) >> 8) & 0xff)
#define WIFCONTINUED(status) ((status) == 0xffff)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_WAIT_H */
