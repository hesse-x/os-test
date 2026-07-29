/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * musl pthread glue — hidden internal aliases musl pthread calls.
 *
 * musl's pthread is compiled verbatim from third_party/musl (musl_pthread
 * sub-library), but it does not stand alone: the thread code calls a set of
 * *hidden* internal symbols (names not exposed in musl's public headers)
 * that normally live in musl's libc. We keep our own libc (printf/malloc/
 * string/FILE/...) and only replace pthread, so those hidden symbols must be
 * provided here as thin forwarders to our existing syscall wrappers.
 *
 * Hidden (visibility("hidden")) so they satisfy musl's internal references
 * without polluting the exported libc ABI.
 *
 * The trickiest is __libc_sigaction: musl's user `struct sigaction` carries a
 * 128-byte sa_mask (musl _NSIG=65 → sigset_t = unsigned long[16]), while our
 * kernel sigaction carries an 8-byte sa_mask (sigset_t = uint64_t, signals
 * 1..64). The kernel's sys_sigaction rejects the call unless arg4 == 8. musl's
 * own __libc_sigaction (src/signal/sigaction.c) is therefore NOT compiled —
 * it would build a 40-byte k_sigaction and pass mask-size 16. Instead we
 * convert field-by-field: handler/flags/restorer pass through, the low 8
 * bytes of the 128-byte mask are the signals-1..64 set our kernel reads.
 *
 * musl's rt_sigprocmask path (src/signal/block.c: __block_all_sigs etc.)
 * already passes arg4 = _NSIG/8 = 8 and a buffer whose low 8 bytes hold
 * signals 1..64, so it is kernel-compatible with NO glue.
 *
 * restorer: musl's x86_64/restore.s defines __restore_rt (mov $15; syscall =
 * rt_sigreturn). The kernel signal delivery (signal.c deliver_signal) honors a
 * non-NULL sa_restorer; we set it to __restore_rt so rt_sigreturn returns to
 * the right place after a musl-installed handler (the pthread_cancel handler
 * in particular).
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <syscall.h>
#include <time.h> // IWYU pragma: keep

#include <sys/types.h>
// #include <bits/signal.h>
#include <xos/syscall_asm.h>
#include <xos/syscall_nums.h>
/* musl internal TCB / __libc touchpoints are reached via these forwarders only;
 * we do NOT include musl private headers here (kept free-standing on our libc
 * side). pthread_impl.h would pull musl's struct pthread layout into libc.a,
 * which we deliberately avoid. */

/* __restore_rt is defined by musl's src/signal/x86_64/restore.s (compiled into
 * the musl_pthread sub-library). Forward-declare so __libc_sigaction can hand
 * it to the kernel as sa_restorer. */
extern void __restore_rt(void);

/* ---- kernel struct sigaction wire layout (include/uapi/xos/signal.h) ----
 * field order: handler union, sa_mask (8-byte mask = signals 1..64),
 * sa_flags, sa_restorer — total 32 bytes. We MUST NOT use the userspace
 * sigset_t here: after the musl header switch, userspace sigset_t is musl's
 * 128-byte struct (unsigned long[16]), not the kernel's 8-byte mask. So the
 * wire mask is an explicit uint64_t, and the mask-size arg passed to
 * rt_sigaction is the literal 8 (sizeof our 8-byte kernel mask), NOT
 * sizeof(sigset_t) (which would be 128 in this TU and the kernel would
 * reject).
 *
 * Member names below deliberately AVOID sa_handler/sa_sigaction — musl's
 * <signal.h> #defines those as macros (sa_handler → __sa_handler.sa_handler),
 * which would corrupt these struct declarations. Use _handler/_sigaction. */
struct k_sigaction {
  union {
    void (*_handler)(int);
    void (*_sigaction)(int, siginfo_t *, void *);
  } _u;
  uint64_t sa_mask; /* kernel's 8-byte mask (signals 1..64) */
  int sa_flags;
  void (*sa_restorer)(void);
};

/* Our user <signal.h> (now musl's) defines sigset_t as a 128-byte struct with
 * __bits[16]; the *callers* of __libc_sigaction are musl files compiled with
 * musl's <signal.h>, so the `struct sigaction *` they pass is musl's 152-byte
 * layout (handler union, sa_mask=128B, sa_flags, sa_restorer). Access it
 * through this musl-layout view to read fields portably, then build our
 * 32-byte kernel-layout k_sigaction. Member names avoid the sa_handler macro
 * for the same reason as k_sigaction above. */
struct musl_sigaction {
  union {
    void (*_handler)(int);
    void (*_sigaction)(int, siginfo_t *, void *);
  } _u;
  /* musl sigset_t = unsigned long __bits[16] (128 bytes). Low long =
   * sigs 1..64. */
  unsigned long sa_mask_bits[16];
  int sa_flags;
  void (*sa_restorer)(void);
};

/* Musl does not define a `hidden` attribute macro; its internal symbols get
 * hidden visibility via ATTR_LIBC_VISIBILITY / version scripts. Our glue lives
 * in libc.a/libc.so, so mark the forwarders hidden to keep them out of the
 * exported libc ABI (musl_pthread.a resolves them by plain global name). */
#define MUSL_HIDDEN __attribute__((visibility("hidden")))

MUSL_HIDDEN int __libc_sigaction(int sig, const void *restrict sa_v,
                                 void *restrict old_v) {
  const struct musl_sigaction *sa = (const struct musl_sigaction *)sa_v;
  struct musl_sigaction *old = (struct musl_sigaction *)old_v;

  struct k_sigaction ksa, ksa_old;
  if (sa) {
    ksa._u._handler = sa->_u._handler;
    ksa.sa_flags = sa->sa_flags | SA_RESTORER;
    ksa.sa_restorer = (sa->sa_flags & SA_SIGINFO) ? __restore_rt : __restore_rt;
    /* Truncate musl's 128-byte mask to our 8-byte mask. The low unsigned long
     * holds signals 1..64 (bit sig-1), exactly our uint64_t layout. */
    ksa.sa_mask = (uint64_t)sa->sa_mask_bits[0];
  }

  /* Mask-size arg is the KERNEL's 8-byte sigset_t, NOT sizeof(userspace
   * sigset_t) (=128 under musl). The kernel's sys_sigaction rejects anything
   * other than 8 here. */
  int64_t r = __syscall4(SYS_RT_SIGACTION, (int64_t)sig,
                         (int64_t)(uintptr_t)(sa ? &ksa : 0),
                         (int64_t)(uintptr_t)(old ? &ksa_old : 0), (int64_t)8);
  if (r < 0) {
    errno = -(int)r;
    return -1;
  }

  if (old) {
    old->_u._handler = ksa_old._u._handler;
    old->sa_flags = ksa_old.sa_flags;
    /* Zero the 128-byte musl mask, then lift our 8-byte kernel mask into the
     * low long. */
    for (int i = 0; i < 16; i++)
      old->sa_mask_bits[i] = 0;
    old->sa_mask_bits[0] = (unsigned long)ksa_old.sa_mask;
    old->sa_restorer = ksa_old.sa_restorer;
  }
  return 0;
}

/* musl's __block_all_sigs/__restore_sigs (block.c) call rt_sigprocmask directly
 * with arg4 = _NSIG/8 = 8 and a low-8-byte mask — already kernel-compatible.
 * No glue needed for sigprocmask. */

MUSL_HIDDEN void *__mmap(void *addr, size_t len, int prot, int flags, int fd,
                         off_t off) {
  return sys_mmap(addr, len, prot, flags, fd, off);
}

MUSL_HIDDEN int __munmap(void *addr, size_t len) {
  return sys_munmap(addr, len);
}

MUSL_HIDDEN int __mprotect(void *addr, size_t len, int prot) {
  return sys_mprotect(addr, len, prot);
}

MUSL_HIDDEN int __clock_gettime(clockid_t clk, struct timespec *ts) {
  return sys_clock_gettime((int)clk, ts);
}
