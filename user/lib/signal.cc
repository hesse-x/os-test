#include <signal.h>
#include <string.h>    // memset
#include <stdlib.h>    // abort (declared in stdlib.h)
#include <syscall.h>   // sys_kill, sys_sigaction, sys_sigreturn
#include <unistd.h>    // getpid
#include <pthread.h>   // pthread_sigmask

int kill(int pid, int sig) {
    return sys_kill((int)pid, sig);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    return sys_sigaction(sig, act, oldact);
}

int sigreturn(void) {
    return sys_sigreturn();
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    /* D11：sigprocmask = pthread_sigmask（POSIX 进程级掩码 = 线程级掩码，
     * 本 OS 单线程进程语义一致）。 */
    return pthread_sigmask(how, set, oldset);
}

int raise(int sig) {
    return kill(getpid(), sig);
}

sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction old;
    struct sigaction new_act;
    memset(&new_act, 0, sizeof(new_act));
    new_act.sa_handler = handler;
    new_act.sa_mask = 0;
    new_act.sa_flags = 0;
    if (sigaction(sig, &new_act, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

/* abort（D13）：先 raise(SIGABRT)；若被忽略/捕获，重置 SIGABRT 为 SIG_DFL
 * 再 raise，确保进程终止。 */
void abort(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGABRT);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    raise(SIGABRT);
    /* 仍存活 → handler 拦截了 SIGABRT，重置默认动作再 raise */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGABRT, &dfl, NULL);
    raise(SIGABRT);
    /* 不应到达 */
    _exit(127);
}
