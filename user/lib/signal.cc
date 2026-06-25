#include <signal.h>
#include "common/syscall.h"       // sys_kill, sys_sigaction, sys_sigreturn

int kill(int pid, int sig) {
    return sys_kill((int)pid, sig);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    return sys_sigaction(sig, act, oldact);
}

int sigreturn(void) {
    return sys_sigreturn();
}

sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction old;
    struct sigaction new_act;
    new_act.sa_handler = handler;
    new_act.sa_mask = 0;
    new_act.sa_flags = 0;
    if (sigaction(sig, &new_act, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}
