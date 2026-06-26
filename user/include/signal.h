#ifndef USER_SIGNAL_H
#define USER_SIGNAL_H

#include "common/signal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIG_ERR   ((void (*)(int))-1)

// ===================== SA_* flags (mirror common/signal.h) =====================
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESETHAND 0x08000000
#define SA_NODEFER   0x40000000
#define SA_RESTART   0x10000000

// ===================== Function declarations =====================
int kill(int pid, int sig);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int sigreturn(void);
int raise(int sig);
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t handler);

#ifdef __cplusplus
}
#endif

#endif // USER_SIGNAL_H
