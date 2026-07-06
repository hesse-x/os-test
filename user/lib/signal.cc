/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <pthread.h> // pthread_sigmask
#include <signal.h>
#include <stdlib.h>  // abort (declared in stdlib.h)
#include <string.h>  // memset
#include <syscall.h> // sys_kill, sys_sigaction, sys_sigreturn
#include <unistd.h>  // getpid
#include <xos/signal.h>

int kill(int pid, int sig) { return sys_kill((int)pid, sig); }

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
  return sys_sigaction(sig, act, oldact);
}

int sigreturn(void) { return sys_sigreturn(); }

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  /* D11: sigprocmask = pthread_sigmask (POSIX process-level mask = thread-level
   * mask; in this OS single-threaded processes have equivalent semantics). */
  return pthread_sigmask(how, set, oldset);
}

int raise(int sig) { return kill(getpid(), sig); }

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

/* abort (D13): first raise(SIGABRT); if ignored/caught, reset SIGABRT to
 * SIG_DFL and raise again to ensure the process terminates. */
void abort(void) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGABRT);
  sigprocmask(SIG_UNBLOCK, &set, NULL);
  raise(SIGABRT);
  /* Still alive → a handler intercepted SIGABRT; reset to default action and
   * raise */
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigaction(SIGABRT, &dfl, NULL);
  raise(SIGABRT);
  /* Should not reach here */
  _exit(127);
}
