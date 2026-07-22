/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/bsd/signal.c — Signal delivery and session syscalls
// Extracted from kernel/trap.c (phase 3 step 3.2)

#include "kernel/bsd/signal.h"

#include <stddef.h>

#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h" // KERNEL_VMA_BOUNDARY
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/proc.h"
#include "kernel/bsd/signalfd.h"
#include "kernel/bsd/syscall.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"

#include <xos/errno.h>
#include <xos/signal.h>
#include <xos/socket.h>
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

  // mov rax, SYS_RT_SIGRETURN
  vaddr[0] = 0x48; // REX.W prefix
  vaddr[1] = 0xC7; // MOV r64, imm32
  vaddr[2] = 0xC0; // ModRM: rax
  vaddr[3] = SYS_RT_SIGRETURN;
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
  // S02: prefer a user-supplied restorer (glibc's __restore_rt); fall back to
  // the kernel trampoline page when the action left sa_restorer NULL (this OS's
  // hand-written libc sigaction wrapper does not set one).
  uint64_t tramp =
      sa->sa_restorer ? (uint64_t)sa->sa_restorer : SIG_TRAMPOLINE_ADDR;
  frame.pretcode = tramp;

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

  // S04: record the altstack in effect so rt_sigreturn can restore it and a
  // handler can query it via sigaltstack(NULL,&old). Filled before the stack
  // switch decision below mutates sas_ss_flags.
  frame.uc.uc_stack.ss_sp = proc->sas_ss_sp;
  frame.uc.uc_stack.ss_size = proc->sas_ss_size;
  frame.uc.uc_stack.ss_flags = (proc->sas_ss_flags & SS_DISABLE)
                                   ? SS_DISABLE
                                   : (proc->sas_ss_flags & SS_ONSTACK);

  // Update blocked: mask sa_mask + current signal during handler. S02
  // SA_NODEFER suppresses the automatic add of the current signal.
  sigset_t new_mask = sa->sa_mask;
  if (!(sa->sa_flags & SA_NODEFER))
    new_mask |= (1ULL << (sig - 1));
  new_mask &= ~((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP)));
  proc->proc->sig_blocked |= new_mask;

  // Clear sig_force_info (consumed)
  if (proc->proc->sig_force_info.si_signo == sig)
    proc->proc->sig_force_info.si_signo = 0;

  // S04: SA_ONSTACK delivery — run the handler on the alternate signal stack
  // instead of the current user stack. Linux only switches when the thread is
  // not already running on the altstack (no nested re-use of the same region).
  uint64_t user_rsp;
  if ((sa->sa_flags & SA_ONSTACK) && !(proc->sas_ss_flags & SS_DISABLE) &&
      !(proc->sas_ss_flags & SS_ONSTACK) && proc->sas_ss_size > 0) {
    user_rsp = (uint64_t)proc->sas_ss_sp + proc->sas_ss_size;
    proc->sas_ss_flags |= SS_ONSTACK;
    // Reflect the now-active on-stack state in the saved uc_stack so a handler
    // querying sigaltstack(NULL,&old) sees SS_ONSTACK (matches Linux).
    frame.uc.uc_stack.ss_flags = SS_ONSTACK;
    // SS_AUTODISARM: disable the altstack on entry so a nested SA_ONSTACK
    // signal cannot reuse it (would overflow). rt_sigreturn does NOT re-arm.
    if (proc->sas_ss_flags & SS_AUTODISARM)
      proc->sas_ss_flags |= SS_DISABLE;
  } else {
    user_rsp = tf->rsp;
  }

  // Push sigframe to user stack. SysV AMD64 ABI requires rsp at a function's
  // entry to be 8 mod 16 (the state after a `call` pushed the 8-byte return
  // address). The handler is entered via iretq with [rsp]=pretcode as the
  // return address, so we place that return address at rsp-8 below the
  // 16-aligned frame — making the handler's entry rsp exactly 8 mod 16.
  // Otherwise a handler prologue using aligned SSE (movaps) faults with #GP.
  //
  // Writes go through copy_to_user under the user CR3 (user pages are not
  // mapped in the kernel CR3). If the user stack is exhausted (e.g. unbounded
  // SA_NODEFER re-entry), copy_to_user faults on an unmapped user page and its
  // _ASM_EXTABLE fixup returns -EFAULT instead of panicking the kernel — we
  // then force SIGSEGV on the target (default-terminate) and leave the
  // trapframe unmodified so user mode re-enters and runs the SIGSEGV path.
  user_rsp -= sizeof(struct rt_sigframe);
  user_rsp &= ~0xFULL; // 16-byte aligned base for the frame

  uint64_t saved_cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
  __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
  size_t ctu_frame =
      copy_to_user((void __user *)user_rsp, &frame, sizeof(struct rt_sigframe));
  // Return address sits 8 bytes below the frame; handler entry rsp is now
  // 8 mod 16 (matches a real `call`).
  uint64_t ret_addr = tramp;
  size_t ctu_ret =
      copy_to_user((void __user *)(user_rsp - 8), &ret_addr, sizeof(ret_addr));
  __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");

  if (ctu_frame || ctu_ret) {
    // Stack overflow / unmapped user stack: cannot build the sigframe. Mirror
    // Linux: force SIGSEGV (default-terminate; if a handler is installed it
    // still cannot run with no stack, so this is terminal). Leave the
    // trapframe unmodified so the task re-enters user mode and the forced
    // SIGSEGV is delivered on the next check_pending_signals pass.
    printk(LOG_ERROR,
           "signal: pid=%d sigframe write faulted (frame=%zu ret=%zu) — "
           "user stack exhausted, forcing SIGSEGV\n",
           proc->pid, ctu_frame, ctu_ret);
    force_sig(proc, SIGSEGV, SI_KERNEL, (void *)user_rsp);
    return;
  }

  // Modify trapframe → jump to handler
  tf->rip = (int64_t)sa->__sigaction_handler._sa_handler;
  tf->rsp = user_rsp - 8;

  if (sa->sa_flags & SA_SIGINFO) {
    tf->rdi = (int64_t)sig;
    tf->rsi = (int64_t)(user_rsp + offsetof(struct rt_sigframe, info));
    tf->rdx = (int64_t)(user_rsp + offsetof(struct rt_sigframe, uc));
  } else {
    tf->rdi = (int64_t)sig;
  }
}

// ===================== signal default actions (S01) =====================
// Linux-aligned default-action classification (kernel/signal.c default_action).
typedef enum { DA_TERM, DA_IGN, DA_STOP, DA_CONT, DA_CORE } default_act;

static default_act sig_default_action(int sig) {
  switch (sig) {
  case SIGCHLD:
  case SIGURG:
  case SIGWINCH:
  case SIGPWR:
    return DA_IGN;
  case SIGSTOP:
  case SIGTSTP:
  case SIGTTOU:
  case SIGTTIN:
    return DA_STOP;
  case SIGCONT:
    return DA_CONT;
  case SIGQUIT:
  case SIGILL:
  case SIGABRT:
  case SIGFPE:
  case SIGSEGV:
  case SIGBUS:
  case SIGTRAP:
  case SIGSYS:
  case SIGXCPU:
  case SIGXFSZ:
    return DA_CORE;
  case SIGKILL:
  default:
    return DA_TERM;
  }
}

// Notify the parent of a child state change (stop/continue/exit) by pending
// SIGCHLD. si_code is a CLD_* value; the parent reads the actual status via
// wait4 (the encoding lives in xtask.exit_code), so siginfo only needs the
// pending bit + a best-effort si_code (no CLD_* union member exists yet).
// S02 SA_NOCLDSTOP: the parent's SIGCHLD action may suppress stop/continue
// notifications.
static void notify_parent_sigchld(xtask *child, int sig, int code) {
  struct signal_struct *sig_s = child->proc->signal;
  pid_t ppid = sig_s->parent_pid;
  if (ppid < 0 || ppid >= MAX_PROC)
    return;
  xtask *parent = task_get(ppid);
  if (parent->pid != ppid || !parent->proc)
    return;
  // S02 SA_NOCLDSTOP: skip SIGCHLD on stop/continue when the parent opted out.
  if ((code == CLD_STOPPED || code == CLD_CONTINUED) &&
      (parent->proc->signal->action[SIGCHLD].sa_flags & SA_NOCLDSTOP))
    return;
  __atomic_or_fetch(&parent->proc->sig_pending, SIGMASK(SIGCHLD),
                    __ATOMIC_RELEASE);
  int pcpu = parent->assigned_cpu;
  uint64_t pflags;
  spin_lock_irqsave(&cpu_locals[pcpu].scheduler_lock, &pflags);
  if (parent->state == BLOCKED && parent->wait_event == WAIT_CHILD)
    wake_from_wait(parent);
  spin_unlock_irqrestore(&cpu_locals[pcpu].scheduler_lock, pflags);
}

// Stop the current task (job control). Saves the WIFSTOPPED encoding into
// xtask.exit_code for wait4(WUNTRACED), transitions to STOPPED (not on any
// run_queue, not reapable), notifies the parent, and yields. Never returns to
// user mode — schedule() picks another task. Caller is check_pending_signals
// running on the current task, so `assigned_cpu` is the local CPU.
static void do_stop(xtask *t, int sig) {
  t->exit_code = (sig << 8) | 0x7f; // WIFSTOPPED encoding (wait4 reads this)
  t->stop_signo = sig;
  t->stop_reported = 0;
  int cpu = t->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  sched_timer_queue_cancel(t); // drop any pending timed-wait
  t->state = STOPPED;
  t->wait_event = WAIT_NONE;
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);

  notify_parent_sigchld(t, sig, CLD_STOPPED);
  schedule(); // yield; does not return here for the stopped task
}

// Resume a STOPPED task on SIGCONT. Called from the kill/tgkill delivery path
// (cross-task) and from check_pending_signals when the current task is stopped
// and a SIGCONT is consumed. Clears the one-shot stop report and re-arms the
// run_queue. Notifies the parent of the continue.
void do_cont(xtask *t) {
  if (t->state != STOPPED)
    return;
  int cpu = t->assigned_cpu;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  t->state = READY;
  t->exit_code = 0;
  t->stop_signo = 0;
  t->stop_reported = 0;
  t->wait_event = WAIT_NONE;
  if (list_empty(&t->run_node))
    run_queue_push(cpu, t);
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
  notify_parent_sigchld(t, SIGCONT, CLD_CONTINUED);
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
    deliverable |= (pending & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
    int sig = 0;
    int sig_shared = 0; // 0 = private sig_pending, 1 = shared_pending
    if (deliverable) {
      sig = __builtin_ctzll(deliverable) + 1; // bit index = sig-1
      __atomic_and_fetch(&proc->proc->sig_pending, ~(1ULL << (sig - 1)),
                         __ATOMIC_RELEASE);
    } else {
      // 2. Then check thread-group shared pending
      uint64_t sflags;
      spin_lock_irqsave(&proc->proc->signal->sig_lock, &sflags);
      pending = proc->proc->signal->shared_pending & ~proc->proc->sig_blocked;
      pending |= (proc->proc->signal->shared_pending &
                  ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))));
      if (pending) {
        sig = __builtin_ctzll(pending) + 1; // bit index = sig-1
        sig_shared = 1;
        proc->proc->signal->shared_pending &= ~(1ULL << (sig - 1));
      }
      spin_unlock_irqrestore(&proc->proc->signal->sig_lock, sflags);
    }

    // bug.md Bug 2 (ls hang): even with no signal we must return to user
    // mode, must break to fall through to the preemption point
    // while(need_resched) schedule() after the loop. The original `return`
    // made that point unreachable for all "return to user mode" paths ->
    // need_resched set but never honored -> shell never scheduled.
    if (sig <= 0 || sig >= NSIG)
      break;

    // signalfd: if an open signalfd accepts this signal (and it is not
    // blocked), defer handler delivery — leave the bit pending so the
    // signalfd reader can consume it. Wake any blocked signalfd waiter.
    // The bit was already cleared above; re-set it so signalfd_do_read sees
    // it, then break out (signal stays pending until the signalfd read).
    if (signalfd_consumes(proc->proc, sig)) {
      if (sig_shared) {
        uint64_t sflags;
        spin_lock_irqsave(&proc->proc->signal->sig_lock, &sflags);
        proc->proc->signal->shared_pending |= 1ULL << (sig - 1);
        spin_unlock_irqrestore(&proc->proc->signal->sig_lock, sflags);
      } else {
        __atomic_or_fetch(&proc->proc->sig_pending, 1ULL << (sig - 1),
                          __ATOMIC_RELEASE);
      }
      // The wq belongs to the signalfd file; keep the RCU read-side
      // critical section held across the __wake_up so a sibling thread
      // can't close + free the file (and its wq) in between.
      rcu_read_lock();
      wait_queue_head *wq = signalfd_wq(proc->proc, sig);
      if (wq)
        __wake_up(wq, POLLIN);
      rcu_read_unlock();
      break;
    }

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
      switch (sig_default_action(sig)) {
      case DA_IGN:
        continue;
      case DA_STOP:
        printk(LOG_INFO, "signal: pid=%d stopped by signal %d\n", proc->pid,
               sig);
        do_stop(proc, sig);
        return; // do_stop yields via schedule(); not reached
      case DA_CONT:
        // SIGCONT default action is "resume if stopped, else ignore". A
        // stopped task that consumed a pending SIGCONT resumes here; a running
        // task just drops it.
        if (proc->state == STOPPED) {
          do_cont(proc);
          return;
        }
        continue;
      case DA_CORE:
        printk(LOG_ERROR, "signal: pid=%d terminated by signal %d (core)\n",
               proc->pid, sig);
        do_exit_with_code((sig & 0x7f) | 0x80); // 0x80 = WCOREDUMP bit (S02)
        return;
      case DA_TERM:
      default:
        printk(LOG_ERROR, "signal: pid=%d terminated by signal %d\n", proc->pid,
               sig);
        do_exit_with_code(sig & 0x7f);
        return;
      }
    } else if (sa->__sigaction_handler._sa_handler == SIG_IGN) {
      continue;
    } else {
      deliver_signal(proc, tf, sig, sa);
      // S02 SA_RESETHAND: reset this signal's action to SIG_DFL after delivery
      // (one-shot handler). SIGKILL/SIGSTOP are never user-modifiable so the
      // reset is unconditional here; the action table is shared across the
      // thread group (signal_struct), matching Linux CLONE_SIGHAND semantics.
      if (sa->sa_flags & SA_RESETHAND) {
        uint64_t sflags;
        spin_lock_irqsave(&proc->proc->signal->sig_lock, &sflags);
        proc->proc->signal->action[sig].__sigaction_handler._sa_handler =
            SIG_DFL;
        proc->proc->signal->action[sig].sa_flags = 0;
        spin_unlock_irqrestore(&proc->proc->signal->sig_lock, sflags);
      }
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
  __atomic_or_fetch(&proc->proc->sig_pending, 1ULL << (sig - 1),
                    __ATOMIC_RELEASE);
  __atomic_and_fetch(&proc->proc->sig_blocked, ~(1ULL << (sig - 1)),
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

// Resume a STOPPED target so a pending signal can be processed when it next
// runs. Used for SIGCONT (full resume + parent CLD_CONTINUED) and for SIGKILL
// (resume to run_queue so check_pending_signals runs the DA_TERM exit — the
// target must do_exit in its own context, not the sender's). For other signals
// a STOPPED target stays stopped (Linux: only SIGKILL/SIGCONT wake a stopped
// task).
static void wake_stopped(xtask *target, int sig) {
  if (target->state != STOPPED)
    return;
  if (sig == SIGCONT) {
    do_cont(target); // full resume + CLD_CONTINUED notify
    return;
  }
  if (sig == SIGKILL) {
    // Re-arm the run_queue so the target runs and consumes the SIGKILL pending
    // bit → DA_TERM → do_exit. No CLD_CONTINUED (the next notify is the exit).
    int cpu = target->assigned_cpu;
    uint64_t flags;
    spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
    target->state = READY;
    target->wait_event = WAIT_NONE;
    if (list_empty(&target->run_node))
      run_queue_push(cpu, target);
    spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);
  }
}

void deliver_signal_to(xtask *target, int sig) {
  // SIGCONT to a stopped target resumes it (and must not pend — Linux clears
  // any pending SIGCONT/SIGSTOP pair on delivery). SIGKILL wakes it to exit.
  if (target->state == STOPPED && (sig == SIGCONT || sig == SIGKILL)) {
    if (sig == SIGCONT) {
      // Clear a possibly-pending SIGSTOP first so "SIGCONT then SIGSTOP" does
      // not re-stop immediately (Linux: SIGCONT wins if delivered after).
      __atomic_and_fetch(&target->proc->sig_pending, ~(SIGMASK(SIGSTOP)),
                         __ATOMIC_RELEASE);
      uint64_t sflags;
      spin_lock_irqsave(&target->proc->signal->sig_lock, &sflags);
      target->proc->signal->shared_pending &= ~(SIGMASK(SIGSTOP));
      spin_unlock_irqrestore(&target->proc->signal->sig_lock, sflags);
    }
    wake_stopped(target, sig);
    // SIGCONT does not pend after resuming; SIGKILL still pends so the woken
    // target exits when it runs.
    if (sig == SIGCONT)
      return;
  }
  __atomic_or_fetch(&target->proc->sig_pending, 1ULL << (sig - 1),
                    __ATOMIC_RELEASE);
  // Signals must be able to interrupt any blocking state (including
  // WAIT_FUTEX, the pthread_cancel path); wake_process_any unconditionally
  // wakes any BLOCKED target regardless of event type.
  if (target->state == BLOCKED)
    wake_process_any(target);
}

int pgsignal(pid_t pgid, int sig) {
  int found = 0;
  for (int p = 0; p < MAX_PROC; p++) {
    if (tasks[p] && tasks[p]->pid == p && tasks[p]->proc &&
        tasks[p]->proc->pgid == pgid) {
      found++;
      // sig==0 is an existence/permission probe only; never deliver (see
      // kill_all: SIGMASK(0) is a negative shift → spurious SIGRTMAX).
      if (sig != 0)
        deliver_signal_to(tasks[p], sig);
    }
  }
  return found > 0 ? 0 : -ESRCH;
}

// ===================== BSD syscall: kill =====================
// S03: Linux-aligned permission + scope. NSIG=65 (RT signals 33-64). sig==0
// does existence + permission validation only. euid==0 ≡ CAP_KILL (no real
// capability subsystem yet — recorded as tech debt). pid==-1 broadcasts to
// every sendable process except init and the sender.

// Permission check: may `sender` post `sig` to `target`? Returns 0 / -EPERM.
// (sig is unused today — real CAP_KILL would let root send any signal even to
// a setuid process; the euid==0 short-circuit already grants that.)
static int kill_permitted(xtask *sender, xtask *target, int sig) {
  (void)sig;
  if (!target || !target->proc)
    return -ESRCH;
  if (sender->proc->euid == 0)
    return 0; // root ≡ CAP_KILL
  if (sender->proc->euid == target->proc->uid ||
      sender->proc->euid == target->proc->euid ||
      sender->proc->uid == target->proc->euid)
    return 0;
  return -EPERM;
}

// Deliver to a process (thread-group shared_pending) and wake any blocked
// thread that would accept it. SIGCONT/SIGKILL to a STOPPED leader resume it.
static int deliver_signal_to_process(xtask *leader, int sig) {
  if (leader->state == STOPPED && (sig == SIGCONT || sig == SIGKILL)) {
    deliver_signal_to(leader, sig); // resume path (handles pending/wake)
    return 0;
  }
  uint64_t sflags;
  spin_lock_irqsave(&leader->proc->signal->sig_lock, &sflags);
  leader->proc->signal->shared_pending |= (1ULL << (sig - 1));
  spin_unlock_irqrestore(&leader->proc->signal->sig_lock, sflags);
  if (!(leader->proc->sig_blocked & (1ULL << (sig - 1))) &&
      leader->state == BLOCKED)
    wake_process_any(leader);
  return 0;
}

// kill(-1, sig): signal every sendable process except init (pid<=1) and the
// sender. Returns 0 if at least one was signalled, else -ESRCH.
static int kill_all(xtask *sender, int sig) {
  int found = 0;
  spin_lock(&tasks_lock);
  for (int p = 0; p < MAX_PROC; p++) {
    xtask *t = tasks[p];
    if (!t || t->pid != p || !t->proc)
      continue;
    // Skip init (init_pid, NOT a hardcoded pid<=1 — init is pid=2 in this OS)
    // and the sender.
    if (t->pid == init_pid || t == sender)
      continue;
    // Only one thread per process: signal the leader (t->pid == t->tgid).
    if (t->tgid != t->pid)
      continue;
    if (kill_permitted(sender, t, sig) == 0) {
      found++;
      // sig==0 is an existence/permission probe only — do NOT deliver.
      // (deliver_signal_to_process would compute SIGMASK(0) = 1<<(0-1), a
      // negative shift whose UB sets bit 63 = SIGRTMAX, spuriously killing
      // the target. Linux semantics: sig 0 never pends.)
      if (sig != 0) {
        spin_unlock(&tasks_lock);
        deliver_signal_to_process(t, sig);
        spin_lock(&tasks_lock);
      }
    }
  }
  spin_unlock(&tasks_lock);
  return found > 0 ? 0 : -ESRCH;
}

int64_t sys_kill(int64_t arg1, int64_t arg2, int64_t unused1, int64_t unused2,
                 int64_t unused3, int64_t unused4) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  pid_t pid = (pid_t)arg1;
  int sig = (int)arg2;
  if (sig < 0 || sig >= NSIG)
    return (int64_t)-EINVAL;

  xtask *sender = current_task;

  // sig==0: validate existence + permission only, no delivery.
  if (sig == 0) {
    if (pid > 0) {
      if (pid >= MAX_PROC)
        return (int64_t)-ESRCH;
      xtask *t = task_get(pid);
      if (t->pid != pid || !t->proc)
        return (int64_t)-ESRCH;
      return (int64_t)kill_permitted(sender, t, 0);
    }
    if (pid == 0) {
      pid_t my_pgid = current_proc->pgid;
      if (my_pgid == 0)
        return (int64_t)-ESRCH;
      // existence check: any process in our pgroup
      return (int64_t)pgsignal(my_pgid, 0);
    }
    if (pid == -1)
      return (int64_t)kill_all(sender, 0);
    return (int64_t)pgsignal(-pid, 0);
  }

  if (pid > 0) {
    if (pid >= MAX_PROC)
      return (int64_t)-ESRCH;
    xtask *leader = task_get(pid);
    if (leader->pid != pid || !leader->proc)
      return (int64_t)-ESRCH;
    int p = kill_permitted(sender, leader, sig);
    if (p)
      return (int64_t)p;
    return deliver_signal_to_process(leader, sig);
  }
  if (pid == 0) {
    pid_t my_pgid = current_proc->pgid;
    if (my_pgid == 0)
      return (int64_t)-ESRCH;
    return (int64_t)pgsignal(my_pgid, sig);
  }
  if (pid == -1)
    return (int64_t)kill_all(sender, sig);
  return (int64_t)pgsignal(-pid, sig); // pid < -1: pgid = -pid
}

// ===================== BSD syscall: alarm =====================
// Set a real-time alarm (in seconds). POSIX alarm() is process-wide: the
// deadline lives in the thread-group signal_struct and SIGALRM is deliverable
// to any thread. Returns the seconds remaining of any previously set alarm,
// or 0 if none. seconds==0 cancels a pending alarm.
int64_t sys_alarm(int64_t arg1, int64_t unused2, int64_t unused3,
                  int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  unsigned seconds = (unsigned)arg1;
  uint64_t now = sched_clock();
  struct signal_struct *sig = current_proc->signal;
  uint64_t flags;
  spin_lock_irqsave(&sig->sig_lock, &flags);
  uint64_t old_remaining = 0;
  if (sig->alarm_deadline != 0) {
    uint64_t rem = sig->alarm_deadline > now ? sig->alarm_deadline - now : 0;
    old_remaining = rem / 1000000000ULL;
  }
  uint64_t new_deadline = 0;
  if (seconds == 0)
    sig->alarm_deadline = 0;
  else {
    new_deadline = now + (uint64_t)seconds * 1000000000ULL;
    sig->alarm_deadline = new_deadline;
  }
  spin_unlock_irqrestore(&sig->sig_lock, flags);

  // Alarm expiry is driven by the Xcore timer queue: a blocked thread that
  // borrowed the alarm deadline as its wait_deadline is woken on expiry and
  // alarm_check (the BSD alarm_check_hook) forces SIGALRM. But a thread may
  // already be blocked in an interruptible wait (pipe read/write, …) WITHOUT
  // having borrowed the deadline — e.g. it blocked before this alarm was armed,
  // so it read alarm_deadline==0 and stayed off the timer queue. With no thread
  // of the process on-CPU (the current_task alarm path in trap.c is idle) and
  // none in the timer queue, the alarm would never fire and the blocked thread
  // would hang. Mirror Linux's "arm a process timer that wakes a blocked thread
  // on expiry": for each same-thread-group BLOCKED thread that has no deadline
  // armed yet (wait_deadline==0 → not in any timer queue), borrow the new
  // deadline so the timer queue will wake it and run alarm_check. Threads that
  // already borrowed an (older) deadline stay; on their expiry alarm_check
  // re-reads the live alarm_deadline and no-ops if it was pushed later, after
  // which they re-borrow the current value. Lock order matches
  // deliver_signal_to_process: sig_lock dropped before taking scheduler_lock.
  if (new_deadline != 0) {
    pid_t my_tgid = current_task->tgid;
    for (int i = 0; i < MAX_PROC; i++) {
      xtask *t = tasks[i];
      if (!t || t->pid < 0 || t->tgid != my_tgid || t == current_task)
        continue;
      int cpu = t->assigned_cpu;
      uint64_t sflags;
      spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &sflags);
      if (t->state == BLOCKED && t->wait_deadline == 0) {
        t->wait_deadline = new_deadline;
        sched_timer_queue_insert(cpu, t);
      }
      spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, sflags);
    }
  }
  return (int64_t)old_remaining;
}

// S03: per-process alarm expiry hook (registered as alarm_check_hook). Called
// from the Xcore timer tick (IRQ context) for the current task, and from
// timer-queue timeout wakes for a blocked task. If the thread-group alarm has
// fired, force SIGALRM (deliverable to any thread) and clear the deadline.
void alarm_check(xtask *t, uint64_t now) {
  if (!t || !t->proc || !t->proc->signal)
    return;
  struct signal_struct *sig = t->proc->signal;
  uint64_t flags;
  spin_lock_irqsave(&sig->sig_lock, &flags);
  uint64_t dl = sig->alarm_deadline;
  if (dl != 0 && dl <= now) {
    sig->alarm_deadline = 0;
    spin_unlock_irqrestore(&sig->sig_lock, flags);
    if (force_sig_hook)
      force_sig_hook(t, SIGALRM, SI_KERNEL, NULL);
    return;
  }
  spin_unlock_irqrestore(&sig->sig_lock, flags);
}

// ===================== BSD syscall: pause =====================
// Suspend the caller until a signal is delivered. Always returns -EINTR when
// resumed (or immediately if a signal is already pending).
int64_t sys_pause(int64_t unused1, int64_t unused2, int64_t unused3,
                  int64_t unused4, int64_t unused5, int64_t unused6) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  (void)unused6;
  // If a signal is already pending, do not block.
  if (current_proc->sig_pending != 0 ||
      current_proc->signal->shared_pending != 0)
    return (int64_t)-EINTR;

  // Borrow the process alarm deadline (if armed) as the wake deadline so the
  // timer queue fires SIGALRM while we block. Read under sig_lock to avoid
  // racing sys_alarm.
  uint64_t alarm_dl = 0;
  uint64_t sflags;
  spin_lock_irqsave(&current_proc->signal->sig_lock, &sflags);
  alarm_dl = current_proc->signal->alarm_deadline;
  spin_unlock_irqrestore(&current_proc->signal->sig_lock, sflags);

  int cpu = get_cpu_local()->cpu_id;
  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  current_task->wait_event = WAIT_PAUSE;
  if (alarm_dl != 0) {
    current_task->wait_deadline = alarm_dl;
    sched_timer_queue_insert(cpu, current_task);
  }
  current_task->state = BLOCKED;
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);

  schedule();

  // Woken by a signal (or alarm expiry, which itself forces SIGALRM).
  return (int64_t)-EINTR;
}

// ===================== BSD syscall: tgkill =====================
// S03: Linux-aligned permission + NSIG=65 + sig==0 validation. tgkill targets
// a specific thread (per-task sig_pending); SIGCONT/SIGKILL to a STOPPED target
// resume it (do_cont / wake-to-exit).
int64_t sys_tgkill(int64_t arg1, int64_t arg2, int64_t arg3, int64_t unused1,
                   int64_t unused2, int64_t unused3) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  pid_t tgid = (pid_t)arg1;
  pid_t tid = (pid_t)arg2;
  int sig = (int)arg3;
  if (sig < 0 || sig >= NSIG)
    return (int64_t)-EINVAL;
  if (tid < 0 || tid >= MAX_PROC)
    return (int64_t)-ESRCH;
  xtask *target = task_get(tid);
  if (target->pid != tid || target->tgid != tgid || !target->proc)
    return (int64_t)-ESRCH;
  if (sig == 0)
    return (int64_t)kill_permitted(current_task, target, 0);
  int p = kill_permitted(current_task, target, sig);
  if (p)
    return (int64_t)p;
  // SIGCONT/SIGKILL to a STOPPED target resume it; deliver_signal_to handles
  // the pending/wake bookkeeping (SIGCONT does not pend, SIGKILL pends so the
  // woken target exits).
  if (target->state == STOPPED && (sig == SIGCONT || sig == SIGKILL)) {
    deliver_signal_to(target, sig);
    return 0;
  }
  // Deliver to the thread-level sig_pending (atomic, no sig_lock)
  __atomic_or_fetch(&target->proc->sig_pending, 1ULL << (sig - 1),
                    __ATOMIC_RELEASE);
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

int64_t sys_sigprocmask(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                        int64_t unused2, int64_t unused3) {
  if ((size_t)arg4 != sizeof(sigset_t))
    return (int64_t)-EINVAL;
  int how = (int)arg1;
  const sigset_t *set = (const sigset_t *)arg2;
  sigset_t *oldset = (sigset_t *)arg3;
  xtask *proc = current_task;

  if (set == NULL && oldset == NULL)
    return 0;

  sigset_t old = proc->proc->sig_blocked;

  if (set) {
    uint64_t ptr = (uint64_t)set;
    if (ptr >= KERNEL_VMA_BOUNDARY ||
        ptr + sizeof(sigset_t) > KERNEL_VMA_BOUNDARY)
      return (int64_t)-EFAULT;

    sigset_t newset;
    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
    if (copy_from_user(&newset, set, sizeof(sigset_t))) {
      __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
      return (int64_t)-EFAULT;
    }
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
    proc->proc->sig_blocked &= ~((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP)));
  }

  if (oldset) {
    uint64_t ptr = (uint64_t)oldset;
    if (ptr >= KERNEL_VMA_BOUNDARY ||
        ptr + sizeof(sigset_t) > KERNEL_VMA_BOUNDARY)
      return (int64_t)-EFAULT;

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
    if (copy_to_user(oldset, &old, sizeof(sigset_t))) {
      __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
      return (int64_t)-EFAULT;
    }
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
  }
  return 0;
}

// ===================== BSD syscall: set_tid_address =====================
// S03: store the full 64-bit tid address (was pid_t, truncating higher-half
// user pointers >4GiB and causing #PF on the do_exit clear). clear_tid_addr is
// a user int* the kernel writes 0 to on thread exit (+ futex_wake for join).
int64_t sys_set_tid_address(int64_t arg1, int64_t unused1, int64_t unused2,
                            int64_t unused3, int64_t unused4, int64_t unused5) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  current_task->proc->clear_tid_addr = (void *)(uintptr_t)arg1;
  return (int64_t)current_task->pid; // returns tid
}

// ===================== BSD syscall: arch_prctl =====================
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

int64_t sys_arch_prctl(int64_t arg1, int64_t arg2, int64_t unused1,
                       int64_t unused2, int64_t unused3, int64_t unused4) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
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
int64_t sys_pthread_set_cancel_handler(int64_t arg1, int64_t unused1,
                                       int64_t unused2, int64_t unused3,
                                       int64_t unused4, int64_t unused5) {
  (void)unused1;
  (void)unused2;
  (void)unused3;
  (void)unused4;
  (void)unused5;
  uint64_t handler = (uint64_t)arg1;
  current_task->proc->cancel_handler = handler;
  return 0;
}

// ===================== BSD syscall: sigaction =====================
int64_t sys_sigaction(int64_t arg1, int64_t arg2, int64_t arg3, int64_t arg4,
                      int64_t unused2, int64_t unused3) {
  if ((size_t)arg4 != sizeof(sigset_t))
    return (int64_t)-EINVAL;
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
    if (ptr >= KERNEL_VMA_BOUNDARY ||
        ptr + sizeof(struct sigaction) > KERNEL_VMA_BOUNDARY)
      return (int64_t)-EFAULT;

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
    if (copy_to_user(oldact, &proc->proc->signal->action[sig],
                     sizeof(struct sigaction))) {
      __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
      return (int64_t)-EFAULT;
    }
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
  }

  if (act) {
    uint64_t ptr = (__force uint64_t)act;
    if (ptr >= KERNEL_VMA_BOUNDARY ||
        ptr + sizeof(struct sigaction) > KERNEL_VMA_BOUNDARY)
      return (int64_t)-EFAULT;

    struct sigaction new_act;
    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
    if (copy_from_user(&new_act, act, sizeof(struct sigaction))) {
      __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
      return (int64_t)-EFAULT;
    }
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");

    if (new_act.sa_mask & ((SIGMASK(SIGKILL)) | (SIGMASK(SIGSTOP))))
      return (int64_t)-EINVAL;

    proc->proc->signal->action[sig] = new_act;
    __atomic_and_fetch(&proc->proc->sig_pending, ~(1ULL << (sig - 1)),
                       __ATOMIC_RELEASE);
  }

  return 0;
}

// ===================== BSD syscall: sigreturn =====================
int64_t sys_sigreturn(int64_t unused1, int64_t unused2, int64_t unused3,
                      int64_t unused4, int64_t unused5, int64_t unused6) {
  xtask *proc = current_task;

  uint64_t tf_base = get_cpu_local()->tss_rsp0 - sizeof(trapframe);
  trapframe *tf = (trapframe *)tf_base;

  struct rt_sigframe frame;
  // Delivery places the frame at a 16-aligned base with the return address in
  // the 8 bytes below it; after the handler's `ret` consumes that address,
  // rsp lands exactly on the frame base. So the frame is at tf->rsp directly.
  uint64_t user_rsp = tf->rsp;

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
  proc->proc->sig_blocked &= ~(SIGMASK(SIGKILL));
  proc->proc->sig_blocked &= ~(SIGMASK(SIGSTOP));

  // S04: restore the per-task sigaltstack state saved at delivery. If the
  // handler ran on the altstack (SS_ONSTACK was set in uc_stack), clear it.
  // SS_AUTODISARM disabled the altstack on entry; per Linux, sigreturn does
  // NOT re-arm it — the user must sigaltstack() again. Clearing SS_ONSTACK
  // here leaves the (possibly SS_DISABLE'd) base/size intact.
  if (frame.uc.uc_stack.ss_flags & SS_ONSTACK)
    proc->sas_ss_flags &= ~SS_ONSTACK;

  return 0;
}

// ===================== BSD syscall: sigaltstack (S04) =====================
// Set and/or query the per-thread alternate signal stack. ss==NULL queries
// only; oss==NULL skips the old-value writeback. Mirrors Linux semantics:
//   - user may not set SS_ONSTACK (kernel-only); only SS_DISABLE is accepted
//     alongside the implicit "no flags" case. Any other flag → EINVAL.
//   - SS_DISABLE tears down the altstack regardless of ss_size/ss_sp.
//   - otherwise ss_size must be >= MINSIGSTKSZ and ss_sp must be a user vaddr.
int64_t sys_sigaltstack(int64_t arg1, int64_t arg2, int64_t unused1,
                        int64_t unused2, int64_t unused3, int64_t unused4) {
  const stack_t __user *ss = (const stack_t __user *__force)arg1;
  stack_t __user *oss = (stack_t __user * __force) arg2;
  xtask *t = current_task;

  // Write back the current altstack state first (before any change), so a
  // query-only call (ss==NULL) still reports the pre-call value.
  if (oss) {
    uint64_t ptr = (__force uint64_t)oss;
    if (ptr >= KERNEL_VMA_BOUNDARY ||
        ptr + sizeof(stack_t) > KERNEL_VMA_BOUNDARY)
      return (int64_t)-EFAULT;

    stack_t kold;
    kold.ss_sp = t->sas_ss_sp;
    kold.ss_size = t->sas_ss_size;
    // Report SS_DISABLE when no altstack is installed; otherwise the live
    // flags (kernel may have set SS_ONSTACK if this is called from a handler
    // running on the altstack — rare but Linux exposes it).
    kold.ss_flags = (t->sas_ss_flags & SS_DISABLE)
                        ? SS_DISABLE
                        : (t->sas_ss_flags & SS_ONSTACK);

    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)t->cr3) : "memory");
    if (copy_to_user(oss, &kold, sizeof(stack_t))) {
      __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
      return (int64_t)-EFAULT;
    }
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
  }

  if (!ss)
    return 0; // query-only

  uint64_t sptr = (__force uint64_t)ss;
  if (sptr >= KERNEL_VMA_BOUNDARY ||
      sptr + sizeof(stack_t) > KERNEL_VMA_BOUNDARY)
    return (int64_t)-EFAULT;

  stack_t kss;
  uint64_t saved_cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
  __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)t->cr3) : "memory");
  if (copy_from_user(&kss, ss, sizeof(stack_t))) {
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
    return (int64_t)-EFAULT;
  }
  __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");

  // Reject flags the user may not set. SS_ONSTACK is kernel-only; SS_DISABLE
  // is the only user-settable flag. SS_AUTODISARM is honored below.
  if (kss.ss_flags & ~(SS_DISABLE | SS_AUTODISARM))
    return (int64_t)-EINVAL;

  if (kss.ss_flags & SS_DISABLE) {
    t->sas_ss_sp = NULL;
    t->sas_ss_size = 0;
    // Preserve a user-requested SS_AUTODISARM for a later re-arm, but mark
    // disabled now. Linux: a disabled stack has only SS_DISABLE set.
    t->sas_ss_flags = SS_DISABLE;
    return 0;
  }

  if (kss.ss_size < MINSIGSTKSZ)
    return (int64_t)-ENOMEM;

  // The altstack base must live in user address space.
  uint64_t base = (uint64_t)kss.ss_sp;
  if (base >= KERNEL_VMA_BOUNDARY || base + kss.ss_size > KERNEL_VMA_BOUNDARY)
    return (int64_t)-EFAULT;

  t->sas_ss_sp = kss.ss_sp;
  t->sas_ss_size = kss.ss_size;
  // Armed: clear SS_DISABLE/SS_ONSTACK; keep SS_AUTODISARM so the delivery
  // path can auto-disable on entry.
  t->sas_ss_flags = kss.ss_flags & SS_AUTODISARM;
  return 0;
}

// ===================== BSD syscall: sigpending =====================
// Return the set of pending signals (per-task + thread-group shared), WITHOUT
// filtering by sig_blocked — POSIX sigpending reports all pending signals,
// including those blocked. Distinct from check_pending_signals which filters.
int64_t sys_sigpending(int64_t arg1, int64_t arg2, int64_t unused2,
                       int64_t unused3, int64_t unused4, int64_t unused5) {
  if ((size_t)arg2 != sizeof(sigset_t))
    return (int64_t)-EINVAL;
  sigset_t __user *set = (sigset_t __user *)arg1;
  if (!set)
    return (int64_t)-EFAULT;

  uint64_t ptr = (uint64_t)set;
  if (ptr >= KERNEL_VMA_BOUNDARY ||
      ptr + sizeof(sigset_t) > KERNEL_VMA_BOUNDARY)
    return (int64_t)-EFAULT;

  xtask *proc = current_task;
  uint64_t pending =
      __atomic_load_n(&proc->proc->sig_pending, __ATOMIC_ACQUIRE);
  uint64_t sflags;
  spin_lock_irqsave(&proc->proc->signal->sig_lock, &sflags);
  pending |= proc->proc->signal->shared_pending;
  spin_unlock_irqrestore(&proc->proc->signal->sig_lock, sflags);

  sigset_t out = (sigset_t)pending;
  uint64_t saved_cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
  __asm__ volatile("movq %0, %%cr3" ::"r"((int64_t)proc->cr3) : "memory");
  if (copy_to_user(set, &out, sizeof(sigset_t))) {
    __asm__ volatile("movq %0, %%cr3" ::"r"(saved_cr3) : "memory");
    return (int64_t)-EFAULT;
  }
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
