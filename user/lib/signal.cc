/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <xos/syscall_ext.h>

#include <sys/cdefs.h>

LIBC_EXPORT int kill(int pid, int sig) { return sys_kill((int)pid, sig); }

// __libc_sigaction converts the user-facing musl struct sigaction (152B,
// 128B sa_mask) to this OS's 32-byte kernel wire struct, sets sa_restorer to
// __restore_rt, and issues rt_sigaction with mask-size 8. Defined in
// musl_glue.c. Users now pass musl's struct sigaction (the only definition
// visible in userspace after the musl header switch), so the public
// sigaction() must NOT call sys_sigaction directly (it would hand the kernel
// the 152B musl layout, which mismatches the 32B wire struct the kernel
// reads).
extern "C" int __libc_sigaction(int sig, const struct sigaction *act,
                                struct sigaction *oldact);

LIBC_EXPORT int sigaction(int sig, const struct sigaction *act,
                          struct sigaction *oldact) {
  return __libc_sigaction(sig, act, oldact);
}

/* sigreturn is OS-specific (musl's <signal.h> doesn't declare it), so without
 * an extern "C" declaration the C++ compiler mangles it to _Z9sigreturnv and
 * the libc.map "sigreturn" export won't match. Force C linkage + default
 * visibility. */
LIBC_EXPORT extern "C" int sigreturn(void) { return sys_sigreturn(); }

LIBC_EXPORT int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  /* D11: sigprocmask = pthread_sigmask (POSIX process-level mask = thread-level
   * mask; in this OS single-threaded processes have equivalent semantics). */
  return pthread_sigmask(how, set, oldset);
}

LIBC_EXPORT int sigpending(sigset_t *set) {
  /* POSIX: return the set of pending signals, including those blocked. */
  return sys_sigpending(set);
}

LIBC_EXPORT int sigaltstack(const stack_t *ss, stack_t *old_ss) {
  return sys_sigaltstack(ss, old_ss);
}

LIBC_EXPORT int raise(int sig) { return kill(getpid(), sig); }

LIBC_EXPORT sighandler_t signal(int sig, sighandler_t handler) {
  struct sigaction old;
  struct sigaction new_act;
  memset(&new_act, 0, sizeof(new_act));
  new_act.sa_handler = handler;
  /* sa_mask (musl's 128-byte __sigset_t) and sa_flags stay 0 from memset. */
  if (sigaction(sig, &new_act, &old) < 0)
    return SIG_ERR;
  return old.sa_handler;
}

/* abort is provided by musl src/exit/abort.c (musl_stdlib_objs):
 * raise(SIGABRT),
 * __block_all_sigs, a_crash, raise(SIGKILL), _Exit(127). */
