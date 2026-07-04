#ifndef USER_SIGNAL_H
#define USER_SIGNAL_H

#include "xos/signal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIG_ERR   ((void (*)(int))-1)

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
