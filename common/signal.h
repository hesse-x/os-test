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
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20

#define NSIG      32

// ===================== Default actions =====================
#define SIG_DFL ((void (*)(int))0)   // default action (terminate)
#define SIG_IGN ((void (*)(int))1)   // ignore signal

// ===================== SA_* flags =====================
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESETHAND 0x08000000
#define SA_NODEFER   0x40000000
#define SA_RESTART   0x10000000  // defined but not implemented this phase

// ===================== SI_* codes =====================
#define SI_USER    0
#define SI_KERNEL  128
#define SI_QUEUE   -1

// ===================== SIGSEGV si_code =====================
#define SEGV_MAPERR  1   // address not mapped
#define SEGV_ACCERR  2   // permission violation

// ===================== SIGFPE si_code =====================
#define FPE_INTDIV   1   // integer divide by zero

// ===================== SIGILL si_code =====================
#define ILL_ILLOPC   1   // illegal opcode

#ifndef __ASSEMBLER__

#ifndef COMMON_SIGSET_T
#define COMMON_SIGSET_T
typedef uint64_t sigset_t;  // 64 signals, 1 uint64_t
#endif

// ===================== siginfo_t =====================
// Layout: si_signo + si_errno + si_code (3 ints = 12 bytes)
//         + 4 bytes alignment padding (union is 8-byte aligned)
//         + union _sifields (fills remaining to 128 bytes total)
// _pad inside union guarantees the union is at least 112 bytes,
// so total = 12 + 4(pad) + 112 = 128.
typedef struct siginfo_t {
    int si_signo;
    int si_errno;    // cleared to 0
    int si_code;
    union {
        int _pad[(128 - 3*sizeof(int) - sizeof(int)) / sizeof(int)];  // fill to 128
        struct { int32_t si_pid; int32_t si_uid; } _kill;
        void *si_addr;    // SIGSEGV fault address
    } _sifields;
} siginfo_t;

// ===================== sigaction struct =====================
struct sigaction {
    union {
        void   (*_sa_handler)(int);
        void   (*_sa_sigaction)(int, siginfo_t *, void *);
    } __sigaction_handler;
    sigset_t sa_mask;
    int      sa_flags;
    void   (*sa_restorer)(void);  // kernel ignores this
};
typedef struct sigaction sigaction_t;

// Access union members — these macros translate struct member access
#define sa_handler    __sigaction_handler._sa_handler
#define sa_sigaction  __sigaction_handler._sa_sigaction

// ===================== sigcontext =====================
struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, ss, ds, es, fs, gs;
    uint64_t fs_base, gs_base;
    uint64_t cr2;
    uint64_t _pad1;  // align to 8 bytes
};

// ===================== ucontext_t =====================
struct ucontext_t {
    uint64_t          uc_flags;    // cleared to 0
    struct ucontext_t *uc_link;    // NULL
    sigset_t          uc_sigmask;
    struct sigcontext  uc_mcontext;
};

// ===================== rt_sigframe =====================
struct rt_sigframe {
    uint64_t pretcode;  // = SIG_TRAMPOLINE_ADDR
    struct siginfo_t info;
    struct ucontext_t uc;
};

#endif // __ASSEMBLER__

// Trampoline page: mapped at this fixed user-space address in every process
// (read-only, executable, no NX)
#define SIG_TRAMPOLINE_ADDR  0x50000000ULL

// SYS_SIGRETURN syscall number (from common/syscall.h, but needed by trampoline)
// The trampoline code is:  mov rax, SYS_SIGRETURN; syscall
// SYS_SIGRETURN = 45

#endif // COMMON_SIGNAL_H
