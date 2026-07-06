/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/signal.c — Signal delivery and session syscalls
// Extracted from kernel/trap.c (phase 3 step 3.2)

#include "kernel/bsd/signal.h"
#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/syscall.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include <stddef.h>
#include <xos/errno.h>
#include <xos/signal.h>
#include <xos/syscall_nums.h>

// ===================== Signal trampoline =====================
uint64_t sig_trampoline_phys = 0;

void sig_init() {
  // Allocate one shared physical page for the signal trampoline.
  struct page *page = bfc_alloc_page(1);
  if (!page) {
    printk(LOG_ERROR, "sig_init: failed to allocate trampoline page\n");
    return;
  }
  sig_trampoline_phys = (__force uint64_t)page_to_phys(page);
  uint8_t *vaddr =
      (__force uint8_t *)phys_to_virt((__force phys_addr_t)sig_trampoline_phys);

  // mov rax, SYS_SIGRETURN
  vaddr[0] = 0x48; // REX.W prefix
  vaddr[1] = 0xC7; // MOV r64, imm32
  vaddr[2] = 0xC0; // ModRM: rax
  vaddr[3] = SYS_SIGRETURN;
  vaddr[4] = 0x00;
  vaddr[5] = 0x00;
  vaddr[6] = 0x00;

  // syscall
  vaddr[7] = 0x0F;
  vaddr[8] = 0x05;

  printk(LOG_INFO, "sig_init: trampoline at phys=%lx\n", sig_trampoline_phys);
}

// ===================== Signal delivery =====================

// Deliver a signal with a user-registered handler via sigframe on user stack.
static void deliver_signal(xtask *proc, trapframe *tf, int sig,
                           sigaction_t *sa) {
  struct rt_sigframe frame;
  __memset(&frame, 0, sizeof(frame));
  frame.pretcode = SIG_TRAMPOLINE_ADDR;

  // siginfo
  frame.info.si_signo = sig;
  frame.info.si_errno = 0;
  frame.info.si_code = SI_KERNEL;
  if (proc->proc->sig_force_info.si_signo == sig) {
    frame.info = proc->proc->sig_force_info;
  }

  // sigcontext — fill all GP registers from trapframe
  frame.uc.uc_mcontext.r8 = tf->r8;
  frame.uc.uc_mcontext.r9 = tf->r9;
  frame.uc.uc_mcontext.r10 = tf->r10;
  frame.uc.uc_mcontext.r11 = tf->r11;
  frame.uc.uc_mcontext.r12 = tf->r12;
  frame.uc.uc_mcontext.r13 = tf->r13;
  frame.uc.uc_mcontext.r14 = tf->r14;
  frame.uc.uc_mcontext.r15 = tf->r15;
  frame.uc.uc_mcontext.rdi = tf->rdi;
  frame.uc.uc_mcontext.rsi = tf->rsi;
  frame.uc.uc_mcontext.rbp = tf->rbp;
  frame.uc.uc_mcontext.rbx = tf->rbx;
  frame.uc.uc_mcontext.rdx = tf->rdx;
  frame.uc.uc_mcontext.rax = tf->rax;
  frame.uc.uc_mcontext.rcx = tf->rcx;
  frame.uc.uc_mcontext.rsp = tf->rsp;
  frame.uc.uc_mcontext.rip = tf->rip;
  frame.uc.uc_mcontext.eflags = tf->rflags;
  frame.uc.uc_mcontext.cs = tf->cs;
  frame.uc.uc_mcontext.ss = tf->ss;
  frame.uc.uc_mcontext.cr2 =
      (proc->proc->sig_force_info.si_signo == sig)
          ? (int64_t)proc->proc->sig_force_info._sifields.si_addr
          : 0;

  frame.uc.uc_sigmask = proc->proc->sig_blocked;
  frame.uc.uc_flags = 0;
  frame.uc.uc_link = NULL;

  // Update blocked: mask sa_mask + current signal during handler
  proc->proc->sig_blocked |= sa->sa_mask | (1ULL << sig);
  proc->proc->sig_blocked &= ~(1ULL << SIGKILL);
  proc->proc->sig_blocked &= ~(1ULL << SIGSTOP);

  // Clear sig_force_info (consumed)
  if (proc->proc->sig_force_info.si_signo == sig)
    proc->proc->sig_force_info.si_signo = 0;

  // Push sigframe to user stack (CR3 switch)
  uint64_t user_rsp = tf->rsp - sizeof(struct rt_sigframe);
  user_rsp &= ~0xFULL; // 16-byte aligned

  uint64_t saved_cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
  __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
  __memcpy((void *)user_rsp, &frame, sizeof(struct rt_sigframe));
  __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");

  // Modify trapframe → jump to handler
  tf->rip = (int64_t)sa->__sigaction_handler._sa_handler;
  tf->rsp = user_rsp;

  if (sa->sa_flags & SA_SIGINFO) {
    tf->rdi = (int64_t)sig;
    tf->rsi = (int64_t)(user_rsp + offsetof(struct rt_sigframe, info));
    tf->rdx = (int64_t)(user_rsp + offsetof(struct rt_sigframe, uc));
  } else {
    tf->rdi = (int64_t)sig;
  }
}

// ===================== check_pending_signals =====================
void check_pending_signals(trapframe *tf) {
  if (tf->cs != 0x2B)
    return;

  xtask *proc = current_task;
  if (!proc || !proc->proc)
    return;

  // group_exit check (highest priority)
  if (proc->proc->signal->group_exit) {
    sys_exit_group(proc->proc->signal->group_exit_code, 0, 0, 0, 0, 0);
    return; // unreachable
  }

  while (1) {
    // 1. Check per-task private pending first
    uint64_t pending =
        __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
    uint64_t deliverable = pending & ~proc->proc->sig_blocked;
    deliverable |= (pending & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
    int sig = 0;
    if (deliverable) {
      sig = __builtin_ctzll(deliverable);
      __atomic_and_fetch(&proc->proc->sig_pending, ~(1ULL << sig),
                         __ATOMIC_RELEASE);
    } else {
      // 2. Then check thread-group shared pending
      spin_lock(&proc->proc->signal->sig_lock);
      pending = proc->proc->signal->shared_pending & ~proc->proc->sig_blocked;
      pending |= (proc->proc->signal->shared_pending &
                  ((1ULL << SIGKILL) | (1ULL << SIGSTOP)));
      if (pending) {
        sig = __builtin_ctzll(pending);
        proc->proc->signal->shared_pending &= ~(1ULL << sig);
      }
      spin_unlock(&proc->proc->signal->sig_lock);
    }

    // bug.md Bug 2 (ls hang): even with no signal we must return to user
    // mode, must break to fall through to the preemption point
    // while(need_resched) schedule() after the loop. The original `return`
    // made that point unreachable for all "return to user mode" paths ->
    // need_resched set but never honored -> shell never scheduled.
    if (sig <= 0 || sig >= NSIG)
      break;

    // SIGCANCEL: does not go through the sigaction table; the kernel delivers
    // directly to cancel_handler
    if (sig == SIGCANCEL) {
      uint64_t handler = proc->proc->cancel_handler;
      if (handler == 0) {
        // Death by signal: exit_code is encoded as a Linux wait status
        // (sig & 0x7f). Go through do_exit_with_code rather than sys_exit
        // to avoid sys_exit's (code<<8) encoding misplacing the signal
        // number into the exit status bits. D13.
        do_exit_with_code(sig & 0x7f);
        return;
      }
      sigaction_t sa;
      __memset(&sa, 0, sizeof(sa));
      sa.__sigaction_handler._sa_handler = (void (*)(int))handler;
      sa.sa_mask = 0;
      sa.sa_flags = 0;
      deliver_signal(proc, tf, sig, &sa);
      break; // after delivery, return to user mode to run handler: fall through
             // to preemption point
    }

    sigaction_t *sa = &proc->proc->signal->action[sig];

    if (sa->__sigaction_handler._sa_handler == SIG_DFL) {
      switch (sig) {
      case SIGCHLD:
      case SIGSTOP:
      case SIGTSTP:
      case SIGCONT:
      case SIGWINCH:
        break;
      case SIGKILL:
      case SIGINT:
      case SIGQUIT:
      case SIGHUP:
      case SIGTERM:
      case SIGSEGV:
      case SIGILL:
      case SIGFPE:
      case SIGABRT:
      case SIGBUS:
      case SIGTRAP:
      case SIGPIPE:
      case SIGALRM:
      case SIGUSR1:
      case SIGUSR2:
      case SIGSTKFLT:
      default:
        proc->proc->exit_code = -1;
        printk(LOG_ERROR, "signal: pid=%d terminated by signal %d\n", proc->pid,
               sig);
        sys_exit(-1, 0, 0, 0, 0, 0);
      }
    } else if (sa->__sigaction_handler._sa_handler == SIG_IGN) {
      continue;
    } else {
      deliver_signal(proc, tf, sig, sa);
      break; // after delivery, return to user mode to run handler: fall through
             // to preemption point
    }
  }

  // Reschedule loop: keep calling schedule() until need_resched is cleared.
  // schedule() clears need_resched at entry; if re-set by another IPI/wake
  // before returning to user mode, loop continues until no resched needed.
  while (current_task->need_resched) {
    schedule();
  }
}

// ===================== force_sig =====================
void force_sig(xtask *proc, int sig, int si_code, void *si_addr) {
  __atomic_or_fetch(&proc->proc->sig_pending, 1ULL << sig, __ATOMIC_RELEASE);
  __atomic_and_fetch(&proc->proc->sig_blocked, ~(1ULL << sig),
                     __ATOMIC_RELEASE);

  proc->proc->sig_force_info.si_signo = sig;
  proc->proc->sig_force_info.si_errno = 0;
  proc->proc->sig_force_info.si_code = si_code;
  proc->proc->sig_force_info._sifields.si_addr = si_addr;

  if (proc->proc->signal->action[sig].__sigaction_handler._sa_handler ==
      SIG_IGN) {
    proc->proc->signal->action[sig].__sigaction_handler._sa_handler = SIG_DFL;
  }
}

// ===================== Signal delivery helpers =====================

void deliver_signal_to(xtask *target, int sig) {
  __atomic_or_fetch(&target->proc->sig_pending, 1ULL << sig, __ATOMIC_RELEASE);
  // Signals must be able to interrupt any blocking state (including
  // WAIT_FUTEX, the pthread_cancel path); use wake_process_any instead of
  // the narrow-semantics wake_process (the latter only handles IPC-class
  // waits).
  if (target->state == BLOCKED)
    wake_process_any(target);
}

int pgsignal(pid_t pgid, int sig) {
  int found = 0;
  for (int p = 0; p < MAX_PROC; p++) {
    if (tasks[p] && tasks[p]->pid == p && tasks[p]->proc &&
        tasks[p]->proc->pgid == pgid) {
      deliver_signal_to(tasks[p], sig);
      found++;
    }
  }
  return found > 0 ? 0 : -ESRCH;
}

// ===================== BSD syscall: kill =====================
int64_t sys_kill(int64_t arg1, int64_t arg2, int64_t _u1, int64_t _u2,
                 int64_t _u3, int64_t _u4) {
  pid_t pid = (pid_t)arg1;
  int sig = (int)arg2;
  if (sig < 0 || sig >= NSIG)
    return (int64_t)-EINVAL;
  if (sig == 0)
    return 0;

  if (pid > 0) {
    if (pid >= MAX_PROC)
      return (int64_t)-ESRCH;
    xtask *leader = task_get(pid);
    if (leader->pid != pid || !leader->proc)
      return (int64_t)-ESRCH;
    // Deliver to process-level shared_pending
    spin_lock(&leader->proc->signal->sig_lock);
    leader->proc->signal->shared_pending |= (1ULL << sig);
    spin_unlock(&leader->proc->signal->sig_lock);
    // Wake the leader if signal not blocked and currently blocked
    // (single-thread stage)
    if (!(leader->proc->sig_blocked & (1ULL << sig)) &&
        leader->state == BLOCKED) {
      wake_process_any(leader);
    }
    return 0;
  } else if (pid == 0) {
    pid_t my_pgid = current_proc->pgid;
    if (my_pgid == 0)
      return (int64_t)-ESRCH;
    return (int64_t)pgsignal(my_pgid, sig);
  } else if (pid == -1) {
    return (int64_t)-EPERM;
  } else {
    return (int64_t)pgsignal(-pid, sig);
  }
}

// ===================== BSD syscall: alarm =====================
// Set a real-time alarm (in seconds). Returns the number of seconds remaining
// of any previously set alarm, or 0 if none. seconds==0 cancels a pending
// alarm.
int64_t sys_alarm(int64_t arg1, int64_t, int64_t, int64_t, int64_t, int64_t) {
  unsigned seconds = (unsigned)arg1;
  uint64_t now = sched_clock();
  uint64_t old_remaining = 0;
  if (current_xtask->alarm_deadline != 0) {
    uint64_t rem = current_xtask->alarm_deadline > now
                       ? current_xtask->alarm_deadline - now
                       : 0;
    old_remaining = rem / 1000000000ULL;
  }
  if (seconds == 0) {
    current_xtask->alarm_deadline = 0;
  } else {
    current_xtask->alarm_deadline = now + (uint64_t)seconds * 1000000000ULL;
  }
  return (int64_t)old_remaining;
}

// ===================== BSD syscall: pause =====================
// Suspend the caller until a signal is delivered. Always returns -EINTR when
// resumed (or immediately if a signal is already pending).
int64_t sys_pause(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) {
  // If a signal is already pending, do not block.
  if (current_proc->sig_pending != 0 ||
      current_proc->signal->shared_pending != 0)
    return (int64_t)-EINTR;

  int cpu = get_cpu_local()->cpu_id;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  current_task->wait_event = WAIT_PAUSE;
  // If an alarm is armed, let the timer queue wake us when it fires — the
  // timer_handler delivers SIGALRM on WAIT_PAUSE expiry. Otherwise wait
  // indefinitely for any signal (deliver_signal_to / wake_process_any).
  if (current_xtask->alarm_deadline != 0) {
    current_task->wait_deadline = current_xtask->alarm_deadline;
    sched_timer_queue_insert(cpu, current_task);
  }
  current_task->state = BLOCKED;
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);

  schedule();

  // Woken by a signal (or alarm expiry, which itself forces SIGALRM).
  return (int64_t)-EINTR;
}

// ===================== BSD syscall: tgkill =====================
int64_t sys_tgkill(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1,
                   int64_t _u2, int64_t _u3) {
  pid_t tgid = (pid_t)arg1;
  pid_t tid = (pid_t)arg2;
  int sig = (int)arg3;
  if (sig < 0 || sig >= NSIG)
    return (int64_t)-EINVAL;
  if (sig == 0)
    return 0;
  if (tid < 0 || tid >= MAX_PROC)
    return (int64_t)-ESRCH;
  xtask *target = task_get(tid);
  if (target->pid != tid || target->tgid != tgid || !target->proc)
    return (int64_t)-ESRCH;
  // Deliver to the thread-level sig_pending (atomic, no sig_lock)
  __atomic_or_fetch(&target->proc->sig_pending, 1ULL << sig, __ATOMIC_RELEASE);
  // A signal delivered via tgkill must be able to interrupt any blocking
  // state (including WAIT_FUTEX, pthread_cancel goes through this path).
  if (target->state == BLOCKED)
    wake_process_any(target);
  return 0;
}

// ===================== BSD syscall: sigprocmask =====================
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

int64_t sys_sigprocmask(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1,
                        int64_t _u2, int64_t _u3) {
  int how = (int)arg1;
  const sigset_t *set = (const sigset_t *)arg2;
  sigset_t *oldset = (sigset_t *)arg3;
  xtask *proc = current_task;

  if (set == NULL && oldset == NULL)
    return 0;

  sigset_t old = proc->proc->sig_blocked;

  if (set) {
    uint64_t ptr = (uint64_t)set;
    if (ptr >= 0xFFFFFFFF80000000ULL ||
        ptr + sizeof(sigset_t) > 0xFFFFFFFF80000000ULL)
      return (int64_t)-EFAULT;

    sigset_t newset;
    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
    copy_from_user(&newset, set, sizeof(sigset_t));
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");

    switch (how) {
    case SIG_BLOCK:
      proc->proc->sig_blocked |= newset;
      break;
    case SIG_UNBLOCK:
      proc->proc->sig_blocked &= ~newset;
      break;
    case SIG_SETMASK:
      proc->proc->sig_blocked = newset;
      break;
    default:
      return (int64_t)-EINVAL;
    }
    // SIGKILL/SIGSTOP cannot be blocked
    proc->proc->sig_blocked &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
  }

  if (oldset) {
    uint64_t ptr = (uint64_t)oldset;
    if (ptr >= 0xFFFFFFFF80000000ULL ||
        ptr + sizeof(sigset_t) > 0xFFFFFFFF80000000ULL)
      return (int64_t)-EFAULT;

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
    copy_to_user(oldset, &old, sizeof(sigset_t));
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
  }
  return 0;
}

// ===================== BSD syscall: set_tid_address =====================
int64_t sys_set_tid_address(int64_t arg1, int64_t _u1, int64_t _u2, int64_t _u3,
                            int64_t _u4, int64_t _u5) {
  current_task->proc->clear_tid_addr = (pid_t)arg1;
  return (int64_t)current_task->pid; // returns tid
}

// ===================== BSD syscall: arch_prctl =====================
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

int64_t sys_arch_prctl(int64_t arg1, int64_t arg2, int64_t _u1, int64_t _u2,
                       int64_t _u3, int64_t _u4) {
  (void)_u1;
  (void)_u2;
  (void)_u3;
  (void)_u4;
  int code = (int)arg1;
  uint64_t addr = (uint64_t)arg2;
  switch (code) {
  case ARCH_SET_FS:
    current_task->fs_base = addr;
    wrmsr(MSR_FS_BASE, addr);
    return 0;
  case ARCH_GET_FS:
    return (int64_t)current_task->fs_base;
  default:
    return (int64_t)-EINVAL;
  }
}

// ===================== BSD syscall: pthread_set_cancel_handler
// =====================
int64_t sys_pthread_set_cancel_handler(int64_t arg1, int64_t _u1, int64_t _u2,
                                       int64_t _u3, int64_t _u4, int64_t _u5) {
  (void)_u1;
  (void)_u2;
  (void)_u3;
  (void)_u4;
  (void)_u5;
  uint64_t handler = (uint64_t)arg1;
  current_task->proc->cancel_handler = handler;
  return 0;
}

// ===================== BSD syscall: sigaction =====================
int64_t sys_sigaction(int64_t arg1, int64_t arg2, int64_t arg3, int64_t _u1,
                      int64_t _u2, int64_t _u3) {
  int sig = (int)arg1;
  const struct sigaction __user *act =
      (const struct sigaction __user *__force)arg2;
  struct sigaction __user *oldact = (struct sigaction __user * __force) arg3;

  if (sig < 0 || sig >= NSIG)
    return (int64_t)-EINVAL;
  if (sig == SIGKILL || sig == SIGSTOP)
    return (int64_t)-EINVAL;
  if (sig == SIGCANCEL)
    return (int64_t)-EINVAL;

  xtask *proc = current_task;

  if (oldact) {
    uint64_t ptr = (__force uint64_t)oldact;
    if (ptr >= 0xFFFFFFFF80000000ULL ||
        ptr + sizeof(struct sigaction) > 0xFFFFFFFF80000000ULL)
      return (int64_t)-EFAULT;

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
    copy_to_user(oldact, &proc->proc->signal->action[sig],
                 sizeof(struct sigaction));
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
  }

  if (act) {
    uint64_t ptr = (__force uint64_t)act;
    if (ptr >= 0xFFFFFFFF80000000ULL ||
        ptr + sizeof(struct sigaction) > 0xFFFFFFFF80000000ULL)
      return (int64_t)-EFAULT;

    struct sigaction new_act;
    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
    copy_from_user(&new_act, act, sizeof(struct sigaction));
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");

    if (new_act.sa_mask & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)))
      return (int64_t)-EINVAL;

    proc->proc->signal->action[sig] = new_act;
    __atomic_and_fetch(&proc->proc->sig_pending, ~(1ULL << sig),
                       __ATOMIC_RELEASE);
  }

  return 0;
}

// ===================== BSD syscall: sigreturn =====================
int64_t sys_sigreturn(int64_t _u1, int64_t _u2, int64_t _u3, int64_t _u4,
                      int64_t _u5, int64_t _u6) {
  xtask *proc = current_task;

  uint64_t tf_base = get_cpu_local()->tss_rsp0 - sizeof(trapframe);
  trapframe *tf = (trapframe *)tf_base;

  struct rt_sigframe frame;
  uint64_t user_rsp = tf->rsp - 8;

  uint64_t saved_cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
  __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
  __memcpy(&frame, (void *)user_rsp, sizeof(struct rt_sigframe));
  __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");

  struct sigcontext *sc = &frame.uc.uc_mcontext;
  tf->r8 = sc->r8;
  tf->r9 = sc->r9;
  tf->r10 = sc->r10;
  tf->r11 = sc->r11;
  tf->r12 = sc->r12;
  tf->r13 = sc->r13;
  tf->r14 = sc->r14;
  tf->r15 = sc->r15;
  tf->rdi = sc->rdi;
  tf->rsi = sc->rsi;
  tf->rbp = sc->rbp;
  tf->rbx = sc->rbx;
  tf->rdx = sc->rdx;
  tf->rax = sc->rax;
  tf->rcx = sc->rcx;
  tf->rsp = sc->rsp;
  tf->rip = sc->rip;
  tf->rflags = sc->eflags;
  tf->cs = sc->cs;
  tf->ss = sc->ss;

  proc->proc->sig_blocked = frame.uc.uc_sigmask;
  proc->proc->sig_blocked &= ~(1ULL << SIGKILL);
  proc->proc->sig_blocked &= ~(1ULL << SIGSTOP);

  return 0;
}

// ===================== BSD syscall: sigpending =====================
// Return the set of pending signals (per-task + thread-group shared), WITHOUT
// filtering by sig_blocked — POSIX sigpending reports all pending signals,
// including those blocked. Distinct from check_pending_signals which filters.
int64_t sys_sigpending(int64_t arg1, int64_t _u1, int64_t _u2, int64_t _u3,
                       int64_t _u4, int64_t _u5) {
  (void)_u1;
  (void)_u2;
  (void)_u3;
  (void)_u4;
  (void)_u5;
  sigset_t __user *set = (sigset_t __user *)arg1;
  if (!set)
    return (int64_t)-EFAULT;

  uint64_t ptr = (uint64_t)set;
  if (ptr >= 0xFFFFFFFF80000000ULL ||
      ptr + sizeof(sigset_t) > 0xFFFFFFFF80000000ULL)
    return (int64_t)-EFAULT;

  xtask *proc = current_task;
  uint64_t pending =
      __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
  spin_lock(&proc->proc->signal->sig_lock);
  pending |= proc->proc->signal->shared_pending;
  spin_unlock(&proc->proc->signal->sig_lock);

  sigset_t out = (sigset_t)pending;
  uint64_t saved_cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
  __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
  copy_to_user(set, &out, sizeof(sigset_t));
  __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
  return 0;
}

// ===================== signal_struct lifecycle =====================
struct signal_struct *signal_create(void) {
  struct signal_struct *sig =
      (struct signal_struct *)kmalloc(sizeof(struct signal_struct));
  if (!sig)
    return NULL;
  __memset(sig, 0, sizeof(struct signal_struct));
  refcount_set(&sig->sig_count, 1);
  atomic_set(&sig->thread_count, 1);
  atomic_set(&sig->live_count, 1);
  sig->sig_lock = SPINLOCK_INIT;
  sig->shared_pending = 0;
  sig->group_exit = 0;
  sig->group_exit_code = 0;
  sig->parent_pid = -1;
  return sig;
}

void signal_put(struct signal_struct *sig) {
  if (!sig)
    return;
  if (refcount_dec_and_test(&sig->sig_count)) {
    kfree(sig);
  }
}
