#ifndef USER_SIGNAL_H
#define USER_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

// ===================== Signal numbers (Linux-compatible subset) =====================
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20

#define NSIG      32
#define SIG_ERR   ((void (*)(int))-1)

// ===================== sigaction struct =====================
struct sigaction {
    void   (*sa_handler)(int);
    unsigned long sa_mask;       // uint64_t on x86-64
    int      sa_flags;
};

// ===================== Default actions =====================
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

// ===================== Function declarations =====================
int kill(int pid, int sig);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int sigreturn(void);
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t handler);

#ifdef __cplusplus
}
#endif

#endif // USER_SIGNAL_H
