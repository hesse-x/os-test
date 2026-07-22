/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_SIGNAL_H
#define COMMON_SIGNAL_H

#include <stddef.h>
#include <stdint.h>

// ===================== Signal numbers (Linux-compatible) =====================
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGPWR 29
#define SIGSYS 31
#define SIGCANCEL 32 // used by pthread_cancel (matches Linux glibc)

// S03 lifts NSIG to 65 so RT signals 33-64 can be delivered. SIGRTMIN is the
// kernel-side floor (covers this OS's SIGCANCEL=32); glibc's user-side floor
// (35, reserving 32-34 for libc) is defined in user/include/signal.h.
#define NSIG 65
#define SIGRTMIN 32
#define SIGRTMAX 64

// ===================== Default actions =====================
#define SIG_DFL ((void (*)(int))0) // default action (terminate)
#define SIG_IGN ((void (*)(int))1) // ignore signal

// Bitmask index for sigset_t (uint64, signals 1..64). Linux convention:
// signal N occupies bit (N-1), so SIGRTMAX (64) → bit 63 fits in uint64.
// Both kernel (1ULL<<(sig-1)) and user sigaddset/sigismember must use this.
#define SIGMASK(sig) (1ULL << ((sig) - 1))

// ===================== SA_* flags =====================
// Values align with Linux x86-64. SA_RESETHAND is 0x80000000 (the high bit),
// NOT 0x08000000 — that value belongs to SA_ONSTACK; the old value was a bug
// (it collided with SA_ONSTACK, which S04 now defines).
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO 0x00000004
#define SA_ONSTACK 0x08000000
#define SA_RESETHAND 0x80000000
#define SA_NODEFER 0x40000000
#define SA_RESTORER                                                            \
  0x04000000 // S02: honor sa_restorer as the return trampoline
#define SA_RESTART                                                             \
  0x10000000 // implemented (S02); slow syscalls restart if a
             // delivering handler sets SA_RESTART

// ===================== sigaltstack (S04) =====================
// Alternate signal stack. ss_flags may carry SS_ONSTACK (set only by the
// kernel while a handler runs on the altstack) and SS_DISABLE (no altstack
// installed). SS_AUTODISARM is a user-requested flag that makes the kernel
// disable the altstack on entry so a nested signal cannot reuse it.
typedef struct {
  void *ss_sp;
  int ss_flags;
  size_t ss_size;
} stack_t;

#define SS_ONSTACK 1
#define SS_DISABLE 2
#define SS_AUTODISARM (1u << 31)

#define MINSIGSTKSZ 2048
#define SIGSTKSZ 8192

// ===================== SIGCHLD si_code (CLD_*) =====================
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

// ===================== SI_* codes =====================
#define SI_USER 0
#define SI_KERNEL 128
#define SI_QUEUE -1

// ===================== SIGSEGV si_code =====================
#define SEGV_MAPERR 1 // address not mapped
#define SEGV_ACCERR 2 // permission violation

// ===================== SIGFPE si_code =====================
#define FPE_INTDIV 1 // integer divide by zero

// ===================== SIGILL si_code =====================
#define ILL_ILLOPC 1 // illegal opcode

#ifndef __ASSEMBLER__

#ifndef COMMON_SIGSET_T
#define COMMON_SIGSET_T
typedef uint64_t sigset_t; // 64 signals, 1 uint64_t
#endif

// ===================== siginfo_t =====================
// Layout: si_signo + si_errno + si_code (3 ints = 12 bytes)
//         + 4 bytes alignment padding (union is 8-byte aligned)
//         + union _sifields (fills remaining to 128 bytes total)
// _pad inside union guarantees the union is at least 112 bytes,
// so total = 12 + 4(pad) + 112 = 128.
typedef struct siginfo_t {
  int si_signo;
  int si_errno; // cleared to 0
  int si_code;
  union {
    int _pad[(128 - 3 * sizeof(int) - sizeof(int)) /
             sizeof(int)]; // fill to 128
    struct {
      int32_t si_pid;
      int32_t si_uid;
    } _kill;
    void *si_addr; // SIGSEGV fault address
  } _sifields;
} siginfo_t;

// ===================== sigaction struct =====================
struct sigaction {
  union {
    void (*_sa_handler)(int);
    void (*_sa_sigaction)(int, siginfo_t *, void *);
  } __sigaction_handler;
  sigset_t sa_mask;
  int sa_flags;
  void (*sa_restorer)(void); // user-supplied signal return trampoline (S02);
                             // NULL → kernel SIG_TRAMPOLINE_ADDR
};
typedef struct sigaction sigaction_t;

// Access union members — these macros translate struct member access
#define sa_handler __sigaction_handler._sa_handler
#define sa_sigaction __sigaction_handler._sa_sigaction

// ===================== sigcontext =====================
struct sigcontext {
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
  uint64_t rip, eflags;
  uint16_t cs, ss, ds, es, fs, gs;
  uint64_t fs_base, gs_base;
  uint64_t cr2;
  uint64_t _pad1; // align to 8 bytes
};

// ===================== ucontext_t =====================
// uc_stack records the alternate signal stack active at delivery time (S04).
// rt_sigreturn restores the per-task sigaltstack state from it. The field
// sits between uc_link and uc_sigmask to mirror Linux's x86-64 ucontext_t.
struct ucontext_t {
  uint64_t uc_flags;          // cleared to 0
  struct ucontext_t *uc_link; // NULL
  stack_t uc_stack;           // S04: altstack in effect at delivery
  sigset_t uc_sigmask;
  struct sigcontext uc_mcontext;
};

// ===================== rt_sigframe =====================
struct rt_sigframe {
  uint64_t pretcode; // = SIG_TRAMPOLINE_ADDR
  struct siginfo_t info;
  struct ucontext_t uc;
};

#endif // __ASSEMBLER__

// Trampoline page: mapped at this fixed user-space address in every process
// (read-only, executable, no NX)
#define SIG_TRAMPOLINE_ADDR 0x50000000ULL

// SYS_SIGRETURN syscall number (from xos/syscall.h, but needed by trampoline)
// The trampoline code is:  mov rax, SYS_SIGRETURN; syscall
// SYS_SIGRETURN = 45

#endif // COMMON_SIGNAL_H
