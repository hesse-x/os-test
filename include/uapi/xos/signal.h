/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_SIGNAL_H
#define COMMON_SIGNAL_H

#include <stddef.h>
#include <stdint.h>

// ===================================================================
// This header is shared by the kernel and userspace, but the two see
// DIFFERENT signal ABIs and only the constant block is common:
//
//   - Signal numbers, SA_* flags, si_code constants: identical between
//     this OS and musl. Defined below under #ifndef so that in userspace
//     musl's own <signal.h> definitions win (same values) and we never
//     redefine; in kernel builds (no musl headers) the guards let ours
//     through.
//
//   - Wire-format structs (sigset_t, struct sigaction, siginfo_t,
//     ucontext_t, stack_t, sigcontext): the kernel's 8-byte-mask /
//     32-byte-struct syscall ABI. Userspace uses musl's larger ABIs
//     (128-byte sigset_t, 152-byte struct sigaction) from <signal.h>;
//     musl_glue.c::__libc_sigaction converts between the two at the
//     syscall boundary. Those struct/typedef definitions are therefore
//     KERNEL-ONLY, gated by __KERNEL__ (defined for all kernel objects
//     in kernel_rules.cmake, absent for userspace). Userspace gets them
//     from musl's headers instead.
// ===================================================================

// ===================== Signal numbers (Linux-compatible) =====================
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGILL
#define SIGILL 4
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGABRT
#define SIGABRT 6
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGFPE
#define SIGFPE 8
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGSTKFLT
#define SIGSTKFLT 16
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGCONT
#define SIGCONT 18
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#ifndef SIGTSTP
#define SIGTSTP 20
#endif
#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif
#ifndef SIGURG
#define SIGURG 23
#endif
#ifndef SIGXCPU
#define SIGXCPU 24
#endif
#ifndef SIGXFSZ
#define SIGXFSZ 25
#endif
#ifndef SIGVTALRM
#define SIGVTALRM 26
#endif
#ifndef SIGPROF
#define SIGPROF 27
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif
#ifndef SIGPWR
#define SIGPWR 29
#endif
#ifndef SIGSYS
#define SIGSYS 31
#endif
// SIGCANCEL: this OS's kernel uses 32 for its legacy in-kernel cancel hook
// (signal.c SIGCANCEL=32 special-case). musl uses 33 (SIGCANCEL in musl's
// pthread_impl.h). Both coexist: the kernel keeps 32 (never reached by musl's
// user-space cancel which uses 33). Not #ifndef-guarded — musl does NOT define
// SIGCANCEL in its public <signal.h>, so ours is the only definition users
// see, and the kernel keeps its own 32 here.
#define SIGCANCEL 32

// S03 lifts NSIG to 65 so RT signals 33-64 can be delivered. In userspace
// musl defines NSIG = _NSIG = 65 (same value) — #ifndef lets musl's win.
#ifndef NSIG
#define NSIG 65
#endif
// Kernel-side SIGRTMIN floor (covers this OS's SIGCANCEL=32). musl's user-side
// SIGRTMIN/__libc_current_sigrtmin() reserves 32-34 for libc and is the
// authoritative userspace value; #ifndef lets it win in userspace.
#ifndef SIGRTMIN
#define SIGRTMIN 32
#endif
#ifndef SIGRTMAX
#define SIGRTMAX 64
#endif

// ===================== Default actions =====================
#ifndef SIG_DFL
#define SIG_DFL ((void (*)(int))0) // default action (terminate)
#endif
#ifndef SIG_IGN
#define SIG_IGN ((void (*)(int))1) // ignore signal
#endif

// Bitmask index for the kernel's 8-byte sigset_t (uint64, signals 1..64).
// Linux convention: signal N occupies bit (N-1). Kernel-internal only;
// userspace sigset_t is musl's 128-byte struct and musl's sigaddset/sigismember
// index .__bits directly, so SIGMASK() is not used in userspace.
#define SIGMASK(sig) (1ULL << ((sig) - 1))

// ===================== SA_* flags =====================
// Values align with Linux x86-64 and musl. #ifndef so musl's definitions win
// in userspace (identical values either way).
#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP 0x00000001
#endif
#ifndef SA_NOCLDWAIT
#define SA_NOCLDWAIT 0x00000002
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 0x00000004
#endif
#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif
#ifndef SA_RESETHAND
#define SA_RESETHAND 0x80000000
#endif
#ifndef SA_NODEFER
#define SA_NODEFER 0x40000000
#endif
#ifndef SA_RESTORER
#define SA_RESTORER                                                            \
  0x04000000 // S02: honor sa_restorer as the return trampoline
#endif
#ifndef SA_RESTART
#define SA_RESTART                                                             \
  0x10000000 // S02-style restart: a slow syscall that returns -ERESTART is   \
             // re-executed (rip -= 2, rax = orig nr) when the delivering      \
             // handler has SA_RESTART set; otherwise it surfaces as -EINTR.   \
             // See refact_syscall/02. Never-restart syscalls (pause, sleep,   \
             // nanosleep, poll-with-timeout, futex-with-timeout) return EINTR.
#endif

// ===================== SIGCHLD si_code (CLD_*) =====================
#ifndef CLD_EXITED
#define CLD_EXITED 1
#endif
#ifndef CLD_KILLED
#define CLD_KILLED 2
#endif
#ifndef CLD_DUMPED
#define CLD_DUMPED 3
#endif
#ifndef CLD_TRAPPED
#define CLD_TRAPPED 4
#endif
#ifndef CLD_STOPPED
#define CLD_STOPPED 5
#endif
#ifndef CLD_CONTINUED
#define CLD_CONTINUED 6
#endif

// ===================== SI_* codes =====================
#ifndef SI_USER
#define SI_USER 0
#endif
#ifndef SI_KERNEL
#define SI_KERNEL 128
#endif
#ifndef SI_QUEUE
#define SI_QUEUE -1
#endif

// ===================== SIGSEGV si_code =====================
#ifndef SEGV_MAPERR
#define SEGV_MAPERR 1 // address not mapped
#endif
#ifndef SEGV_ACCERR
#define SEGV_ACCERR 2 // permission violation
#endif

// ===================== SIGFPE si_code =====================
#ifndef FPE_INTDIV
#define FPE_INTDIV 1 // integer divide by zero
#endif

// ===================== SIGILL si_code =====================
#ifndef ILL_ILLOPC
#define ILL_ILLOPC 1 // illegal opcode
#endif

// ------------------------------------------------------------------
// KERNEL-ONLY wire-format types (see top-of-file note).
// ------------------------------------------------------------------
#ifdef __KERNEL__

// ===================== sigaltstack (S04) =====================
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

// Wire layout matching musl arch/x86_64/ksigaction.h EXACTLY:
// handler@0, flags@8, restorer@16, unsigned mask[2]@24 (32 bytes total). musl's
// __libc_sigaction (src/signal/sigaction.c) passes a struct k_sigaction
// across the rt_sigaction syscall, NOT its 152-byte user struct sigaction.
// The kernel copies this 32-byte shape and field-assigns into the internal
// sigaction_t (which keeps its own field order + 8-byte sa_mask). arg4 is
// _NSIG/8 = 8 (musl x86_64); mask[2] is two 32-bit words, not two unsigned
// longs. Distinct from sigaction_t so the in-kernel action[] storage stays
// decoupled from the userspace wire ABI.
struct k_sigaction_wire {
  void (*handler)(int);
  unsigned long flags;
  void (*restorer)(void);
  unsigned mask[2];
};

#ifdef __cplusplus
static_assert(sizeof(struct k_sigaction_wire) == 32,
              "musl x86_64 k_sigaction ABI size");
#else
_Static_assert(sizeof(struct k_sigaction_wire) == 32,
               "musl x86_64 k_sigaction ABI size");
#endif

// Access union members — these macros translate struct member access
#define sa_handler __sigaction_handler._sa_handler
#define sa_sigaction __sigaction_handler._sa_sigaction

// ===================== sigcontext =====================
struct sigcontext {
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
  uint64_t rip, eflags;
  uint16_t cs, gs, fs, __pad0;
  uint64_t err, trapno, oldmask, cr2;
  void *fpstate;
  uint64_t __reserved1[8];
};

// Linux x86-64 signal-frame ABI. Userspace libc indexes uc_mcontext as 23
// general registers and expects a 128-byte sigset followed by fpstate storage.
struct ucontext_t {
  uint64_t uc_flags;
  struct ucontext_t *uc_link;
  stack_t uc_stack;
  struct sigcontext uc_mcontext;
  uint64_t uc_sigmask[16];
  uint64_t __fpregs_mem[64];
};

// ===================== rt_sigframe =====================
struct rt_sigframe {
  uint64_t pretcode; // = SIG_TRAMPOLINE_ADDR
  struct siginfo_t info;
  struct ucontext_t uc;
};

#endif // __ASSEMBLER__

#endif // __KERNEL__

// Trampoline page: mapped at this fixed user-space address in every process
// (read-only, executable, no NX)
#define SIG_TRAMPOLINE_ADDR 0x50000000ULL

// SYS_SIGRETURN syscall number (from xos/syscall.h, but needed by trampoline)
// The trampoline code is:  mov rax, SYS_SIGRETURN; syscall
// SYS_SIGRETURN = 45

#endif /* COMMON_SIGNAL_H */
