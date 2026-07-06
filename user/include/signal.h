/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USER_SIGNAL_H
#define USER_SIGNAL_H

#include <sys/cdefs.h>
#include <xos/signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIG_ERR ((void (*)(int)) - 1)

/* sigprocmask how argument (matches kernel SIG_BLOCK/UNBLOCK/SETMASK) */
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* sigset_t bit operations (sigset_t = uint64_t, 64 signals) */
#define sigemptyset(s) (*(s) = (sigset_t)0)
#define sigfillset(s) (*(s) = (sigset_t) ~(uint64_t)0)
#define sigaddset(s, n) (*(s) |= (sigset_t)1 << (n))
#define sigdelset(s, n) (*(s) &= (sigset_t) ~((uint64_t)1 << (n)))
#define sigismember(s, n) (!!(*(s) & ((sigset_t)1 << (n))))

// ===================== Function declarations =====================
LIBC_EXPORT int kill(int pid, int sig);
LIBC_EXPORT int sigaction(int sig, const struct sigaction *act,
                          struct sigaction *oldact);
LIBC_EXPORT int sigreturn(void);
LIBC_EXPORT int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
LIBC_EXPORT int sigpending(sigset_t *set);
LIBC_EXPORT int raise(int sig);
typedef void (*sighandler_t)(int);
LIBC_EXPORT sighandler_t signal(int sig, sighandler_t handler);

#ifdef __cplusplus
}
#endif

#endif // USER_SIGNAL_H
