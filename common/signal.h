#ifndef COMMON_SIGNAL_H
#define COMMON_SIGNAL_H

#include <stdint.h>

// ===================== Signal numbers (Linux-compatible) =====================
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

// ===================== sigaction struct =====================
// NOTE: User-space struct sigaction (with sa_handler pointer) is the same
// layout for both kernel and user because pointers are 64-bit.
// Kernel copies the struct directly from user space.
#ifndef __ASSEMBLER__

typedef struct sigaction {
    void   (*sa_handler)(int);  // SIG_DFL=0, SIG_IGN=1, or user fn
    uint64_t sa_mask;           // blocked mask during handler (for sigprocmask)
    int      sa_flags;          // SA_RESTART, etc.
} sigaction_t;

// ===================== Default actions =====================
#define SIG_DFL ((void (*)(int))0)   // default action (terminate)
#define SIG_IGN ((void (*)(int))1)   // ignore signal

#endif // __ASSEMBLER__

// Trampoline page: mapped at this fixed user-space address in every process
// (read-only, executable, no NX)
#define SIG_TRAMPOLINE_ADDR  0x50000000ULL

// SYS_SIGRETURN syscall number (from common/syscall.h, but needed by trampoline)
// The trampoline code is:  mov rax, SYS_SIGRETURN; syscall
// SYS_SIGRETURN = 49

#endif // COMMON_SIGNAL_H
