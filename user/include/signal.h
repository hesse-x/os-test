#ifndef USER_SIGNAL_H
#define USER_SIGNAL_H

#include "common/signal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIG_ERR   ((void (*)(int))-1)

// ===================== Function declarations =====================
int kill(int pid, int sig);
int sigaction(int sig, const sigaction_t *act, sigaction_t *oldact);
int sigreturn(void);
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t handler);

#ifdef __cplusplus
}
#endif

#endif // USER_SIGNAL_H
