/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/trap.h"

#include <stddef.h>

#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/extable.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/rcu.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include <xos/errno.h>
#include <xos/page.h>
#include <xos/signal.h>
#include <xos/syscall_nums.h>

// ===================== IRQ handler registry =====================
#define MAX_IRQ_HANDLERS 256
static irq_handler_fn irq_handlers[MAX_IRQ_HANDLERS];

// ===================== IRQ owner (user-space driver binding)
// =====================
pid_t irq_owner[MAX_IRQ_HANDLERS];

// Helper: check if a kernel ISR is registered for this IRQ vector
int irq_has_handler(int irq) {
  if (irq < 0 || irq >= MAX_IRQ_HANDLERS)
    return 0;
  return irq_handlers[irq] != NULL;
}

void irq_register(int vec, irq_handler_fn fn) {
  if (vec >= 0 && vec < MAX_IRQ_HANDLERS) {
    irq_handlers[vec] = fn;
  }
}

void irq_unregister(int vec) {
  if (vec >= 0 && vec < MAX_IRQ_HANDLERS) {
    irq_handlers[vec] = NULL;
  }
}

// ===================== Hook variables (BSD layer registers during init)
// ===================== Defined here; declarations in kernel/xcore/trap.h
signal_check_fn signal_check_hook = NULL;
fault_handler_fn fault_handler = NULL;
reap_fn reap_hook = NULL;
proc_reap_fn proc_reap_hook = NULL;
devtmpfs_cleanup_fn devtmpfs_cleanup_hook = NULL;
syscall_dispatch_fn syscall_dispatch_hook = NULL;
signal_pending_fn signal_pending_hook = NULL;
force_sig_fn force_sig_hook = NULL;
timer_poll_fn timer_poll_hook = NULL;
timerfd_tick_fn timerfd_tick_hook = NULL;

// ===================== Trap dispatch =====================
static uint64_t tick = 0;

// #NM handler: Device Not Available -- lazy FPU context switch
void fpu_lazy_switch(xtask *t) {
  uint64_t cr0;
  __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
  if (!(cr0 & (1ULL << 3)))
    return; // TS already clear, nothing to handle

  // Nesting detection: normal #NM fires once, clts returns immediately.
  // If the handler triggers another #NM (typical cause: fxrstor/fxsave
  // executed with TS=1), it would nest and blow the kernel stack, causing
  // #DF.  Panic immediately with a clear message, don't let #DF destroy
  // the scene.
  cpu_local *cl = get_cpu_local();
  if (++cl->nm_nesting_depth > 1) {
    panic("#NM nested (depth=%d) — fxrstor/fxsave executed with CR0.TS=1? "
          "use kernel_fpu_save/restore helpers (they clts first)\n",
          cl->nm_nesting_depth);
  }

  if (t->fpu_page) {
    ASSERT(t->fpu_page->status == PAGE_USED);
    void *fpu_data =
        (void *)(__force uintptr_t)phys_to_virt(page_to_phys(t->fpu_page));
    // Defense: fxrstor source must be a BFC data page virtual address
    // (see same ASSERT in fpu_context_switch)
    uint64_t vma_start __attribute__((unused)) =
        (__force uint64_t)phys_to_virt(0);
    ASSERT((uint64_t)fpu_data >= vma_start &&
           (uint64_t)fpu_data < vma_start + total_page_frames * PAGE_SIZE);
    kernel_fpu_restore(fpu_data); // internally clts then fxrstor
  } else {
    // fpu_page == NULL: thread never used FPU, just clear TS
    __asm__ volatile("clts");
  }
  cl->nm_nesting_depth = 0;
}

// ===================== COW fault resolution =====================
__attribute__((no_sanitize("kernel-address"))) static void
resolve_cow_fault(xtask *task, uint64_t *pte, uint64_t fault_addr) {
  uint64_t old_phys = *pte & PTE_PHYS_MASK;
  struct page *old_page = &bfc_frames[PHY_TO_PAGE(old_phys)];

  if (refcount_read(&old_page->p_refcount) == 1) {
    // Only owner: restore write permission without copy
    *pte = (*pte & ~PTE_COW) | PTE_RW;
    *pte &= ~PTE_DIRTY;
    invlpg(fault_addr);
    return;
  }

  // Shared page: allocate new page and copy content
  struct page *new_page = bfc_alloc_page(1);
  if (!new_page) {
    if (force_sig_hook)
      force_sig_hook(task, SIGSEGV, SEGV_MAPERR, (void *)fault_addr);
    return;
  }
  uint64_t new_phys = (__force uint64_t)page_to_phys(new_page);
  __memcpy((__force void *)phys_to_virt((__force phys_addr_t)new_phys),
           (__force void *)phys_to_virt((__force phys_addr_t)old_phys),
           PAGE_SIZE);

  // Update PTE: new physical page, writable, clear COW/DIRTY, preserve
  // NX/ACCESSED
  *pte = new_phys | PTE_PRESENT | PTE_RW | PTE_USER | (*pte & PTE_NX) |
         (*pte & PTE_ACCESSED);

  (void)refcount_dec_and_test(
      &old_page
           ->p_refcount); // decrement, don't free (new page already allocated)
  invlpg(fault_addr);
}

// External symbols bracketing the __ex_table section (defined in linker.ld).
extern const struct exception_table_entry __start___ex_table[];
extern const struct exception_table_entry __stop___ex_table[];

// Linear scan of the exception table for an entry whose .insn == tf->rip.
// Returns the .fixup RIP on match, or 0 on miss.
uint64_t fixup_exception(trapframe *tf) {
  const struct exception_table_entry *e = __start___ex_table;
  const struct exception_table_entry *end = __stop___ex_table;
  for (; e < end; e++) {
    if (e->insn == (uint64_t)tf->rip)
      return e->fixup;
  }
  return 0;
}

// Auto-restore cur_tf on any return from trap_dispatch.  Without this, a
// timer IRQ that fires during syscall execution (sti) overwrites cur_tf
// with the IRQ's trapframe address.  After IRETQ pops the IRQ trapframe,
// cur_tf still points to the now-invalid IRQ stack slot.  cleanup attribute
// ensures cur_tf is restored on every return path, even early returns from
// COW/exception handlers.
static void restore_cur_tf(void *arg) {
  trapframe **saved = (trapframe **)arg;
  get_cpu_local()->cur_tf = *saved;
}

void trap_dispatch(trapframe *tf) {
  trapframe *saved_cur_tf __attribute__((cleanup(restore_cur_tf))) =
      get_cpu_local()->cur_tf;
  get_cpu_local()->cur_tf = tf;
  // Hardware IRQ: check user-space driver binding first
  if (tf->trapno >= 32 && tf->trapno < MAX_IRQ_HANDLERS &&
      __atomic_load_n(&irq_owner[tf->trapno], __ATOMIC_ACQUIRE) >= 0) {
    pid_t owner_pid = __atomic_load_n(&irq_owner[tf->trapno], __ATOMIC_ACQUIRE);
    // Direct index by PID — no scan needed
    if (owner_pid >= 0 && owner_pid < MAX_PROC) {
      xtask *target = task_get(owner_pid);

      // Enqueue RECV_IRQ message to target's recv queue
      spin_lock(&target->recv_lock);
      uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
      if (next != target->recv_tail) { // drop if full
        recv_msg *slot = (recv_msg *)target->recv_buf[target->recv_head];
        slot->type = RECV_IRQ;
        slot->src = tf->trapno;
        target->recv_head = next;
      }
      spin_unlock(&target->recv_lock);

      // Wake target if in WAIT_RECV
      int target_cpu = target->assigned_cpu;
      uint64_t flags;
      spin_lock_irqsave(&cpu_locals[target_cpu].scheduler_lock, &flags);
      if (target->pid == owner_pid && target->state == BLOCKED &&
          target->wait_event == WAIT_RECV) {
        wake_from_wait(target);
      }
      spin_unlock_irqrestore(&cpu_locals[target_cpu].scheduler_lock, flags);
    }
    lapic_eoi();
    return;
  }

  // Check registered handler
  if (tf->trapno < MAX_IRQ_HANDLERS && irq_handlers[tf->trapno] != NULL) {
    irq_handlers[tf->trapno](tf);
    return;
  }

  // Default: timer EOI
  if (tf->trapno == LAPIC_TIMER_VECTOR) {
    tick++;
    lapic_eoi();
    return;
  }

  // Other hardware IRQ: send EOI
  if (tf->trapno >= 32 && tf->trapno <= 127) {
    lapic_eoi();
    return;
  }

  // Early COW fault resolution — silently handle recoverable COW faults
  // before any error logging, to avoid log noise from benign page faults.
  if (tf->trapno == 14 && current_task) {
    uint64_t fault_addr = read_cr2();
    uint64_t error_code = tf->err_code;
    bool is_write = error_code & 2;
    bool is_present = error_code & 1;
    if (is_present && is_write) {
      uint64_t *pte = lookup_pte(current_task->mm->cr3, fault_addr);
      if (pte && (*pte & PTE_COW)) {
        resolve_cow_fault(current_task, pte, fault_addr);
        return; // COW resolved, retry faulting instruction
      }
    }
    // BSD layer file fault handler (e.g., mmap file page-in)
    if (fault_handler && current_task->proc) {
      if (fault_handler(fault_addr, current_task)) {
        return; // BSD layer handled the fault
      }
    }
  }

  // CPU exception: kill user process, halt for kernel exceptions
  // vector 7: #NM (Device Not Available) — lazy FPU switch
  if (tf->trapno == 7) {
    printk(LOG_DEBUG, "#NM (eager FPU fallback) pid=%d\n",
           current_task ? current_task->pid : -1);
    fpu_lazy_switch(current_task);
    return;
  }
  if (tf->trapno == 14) {
    uint64_t cr2;
    __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));
    printk(LOG_ERROR, "PAGE FAULT: fault addr=0x%016lX", cr2);
    // DIAG: detect cross-process physical-page overlap. The faulting rip's
    // page (libc.so .text) was overwritten at runtime; suspect a physical
    // page mapped into two address spaces. Walk all tasks, look up the PTE
    // for the faulting rip's page, record which pids map the SAME physical
    // page. If >1 pid maps it → double allocation in bfc_alloc_page.
    {
      uint64_t rip_page = tf->rip & ~0xFFF;
      uint64_t my_phys = 0;
      if (current_task) {
        uint64_t *pte = lookup_pte(current_task->cr3, rip_page);
        if (pte)
          my_phys = *pte & 0x000FFFFFFFFFF000ULL;
      }
      printk(LOG_ERROR, "\n  OVERLAP-DIAG: rip_page=0x%lX phys=0x%lX", rip_page,
             my_phys);
      if (my_phys) {
        int hits = 0;
        for (int i = 0; i < MAX_PROC; i++) {
          xtask *t = tasks[i];
          if (!t || !t->cr3)
            continue;
          // skip kernel/idle
          if (t->pid == 0)
            continue;
          uint64_t *pte = lookup_pte(t->cr3, rip_page);
          if (pte && (*pte & 0x000FFFFFFFFFF000ULL) == my_phys) {
            printk(LOG_ERROR, " pid%d@cr3=0x%lX", t->pid, t->cr3);
            hits++;
          }
        }
        printk(LOG_ERROR, " (total %d mapping this phys page)\n", hits);
      } else {
        printk(LOG_ERROR, " (rip_page not mapped in current task)\n");
      }
      // DIAG2: full walk of CURRENT task's page table, find ALL virtual
      // addresses mapping to my_phys (same-process alias). If the .text phys
      // page is also mapped at a .data vaddr, a write to .data corrupts .text.
      if (my_phys && current_task) {
        uint64_t *pml4 =
            (uint64_t *)phys_to_virt((__force phys_addr_t)current_task->cr3);
        int aliases = 0;
        for (int i4 = 0; i4 < 512; i4++) {
          if (!(pml4[i4] & 1))
            continue;
          uint64_t *pdpt = (uint64_t *)phys_to_virt(
              (__force phys_addr_t)(pml4[i4] & 0x000FFFFFFFFFF000ULL));
          for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & 1))
              continue;
            if (pdpt[i3] & 0x80) // 1GB huge page
              continue;
            uint64_t *pd = (uint64_t *)phys_to_virt(
                (__force phys_addr_t)(pdpt[i3] & 0x000FFFFFFFFFF000ULL));
            for (int i2 = 0; i2 < 512; i2++) {
              if (!(pd[i2] & 1))
                continue;
              if (pd[i2] & 0x80) // 2MB huge page
                continue;
              uint64_t *pt = (uint64_t *)phys_to_virt(
                  (__force phys_addr_t)(pd[i2] & 0x000FFFFFFFFFF000ULL));
              for (int i1 = 0; i1 < 512; i1++) {
                if (!(pt[i1] & 1))
                  continue;
                if ((pt[i1] & 0x000FFFFFFFFFF000ULL) == my_phys) {
                  uint64_t va = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                  if (i4 >= 256)
                    va |= 0xFFFF000000000000ULL;
                  printk(LOG_ERROR, "\n    ALIAS va=0x%lX pte=0x%lX", va,
                         pt[i1]);
                  aliases++;
                }
              }
            }
          }
        }
        printk(LOG_ERROR, "\n    ALIAS total=%d vaddr(s) map phys 0x%lX\n",
               aliases, my_phys);
      }
    }
    // err code decoding: speed up NX/write-protect/page-not-present diagnosis
    // bit0: 0=not present 1=protection violation
    // bit1: 0=read 1=write
    // bit2: 0=kernel 1=user
    // bit4: 0=data access 1=instruction fetch (typical NX)
    {
      const char *acc =
          (tf->err_code & 0x10) ? "instruction fetch" : "data access";
      const char *cause =
          (tf->err_code & 0x1) ? "protection violation" : "page not present";
      const char *rw = (tf->err_code & 0x2) ? "write" : "read";
      const char *ring = (tf->err_code & 0x4) ? "user" : "kernel";
      printk(LOG_WARN, "  [%s %s %s, %s] err=0x%lX\n", ring, rw, acc, cause,
             tf->err_code);
    }
    printk(LOG_WARN,
           "  hint: NX(instruction fetch/protection) → check PROT_EXEC; "
           "not present → check mapping/ptr; "
           "write/protection → check PTE_RW / COW / __user ptr\n");
  } else if (tf->trapno == 6) {
    printk(LOG_ERROR, "UNDEFINED OPCODE");
    printk(LOG_WARN,
           "  hint: user SSE/SSE2 needs CR4.OSFXSR (bit 9) + CR0.TS clear; "
           "kernel #UD often illegal insn or wrong CS\n");
  } else if (tf->trapno == 13) {
    printk(LOG_ERROR, "GENERAL PROTECTION");
    printk(LOG_WARN,
           "  hint: check segment selectors / IOPL / non-canonical MSR or RIP, "
           "and RPL/TI bits of any segment load\n");
#ifdef SANITIZER
#include "kernel/xcore/mem/kasan.h" // kasan_shadow_exists (SANITIZER #PF diagnostics)
    if (kasan_shadow_exists()) {
      uint64_t fault_addr;
      __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));
      printk(LOG_WARN,
             "\n  KASAN: possible shadow access to non-canonical address");
      printk(LOG_WARN, "\n  Check if __user pointer was used without "
                       "copy_from_user/to_user");
    }
#endif
  } else {
    printk(LOG_ERROR, "EXCEPTION: vector 0x%016lX", tf->trapno);
  }
  printk(LOG_ERROR,
         "\n  rip=0x%016lX cs=0x%016lX rfl=0x%016lX rsp=0x%016lX ss=0x%016lX",
         tf->rip, tf->cs, tf->rflags, tf->rsp, tf->ss);
  printk(LOG_ERROR, "\n  rax=0x%016lX rbx=0x%016lX rcx=0x%016lX rdx=0x%016lX",
         tf->rax, tf->rbx, tf->rcx, tf->rdx);
  printk(LOG_ERROR, "\n  rsi=0x%016lX rdi=0x%016lX rbp=0x%016lX r08=0x%016lX",
         tf->rsi, tf->rdi, tf->rbp, tf->r8);
  printk(LOG_ERROR, "\n  r09=0x%016lX r10=0x%016lX r11=0x%016lX r12=0x%016lX",
         tf->r9, tf->r10, tf->r11, tf->r12);
  printk(LOG_ERROR, "\n  r13=0x%016lX r14=0x%016lX r15=0x%016lX err=0x%016lX",
         tf->r13, tf->r14, tf->r15, tf->err_code);
  uint64_t cr3;
  __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
  printk(LOG_ERROR, "\n  cr3=0x%016lX", cr3);
  if (current_task) {
    printk(LOG_ERROR, " pid=%d proc_cr3=0x%016lX", current_task->pid,
           current_task->cr3);
  }
  printk(LOG_ERROR, "\n");

  dump_stack_trace();

  // === DEBUG: dump faulting instruction bytes and kernel stack ===
  if (tf->cs == 0x08) {
    // Dump 16 bytes at faulting rip (what "code" is the CPU trying to execute?)
    printk(LOG_DEBUG, "  RIP bytes:");
    uint8_t *rip_ptr = (uint8_t *)tf->rip;
    for (int i = 0; i < 16 && rip_ptr; i++) {
      printk(LOG_DEBUG, " %02X", rip_ptr[i]);
    }
    printk(LOG_DEBUG, "\n");

    // Dump current_task kernel stack top area (trapframe + switch_frame)
    if (current_task && current_task->k_stack_top) {
      uint64_t stack_base = current_task->k_stack_top - KERNEL_STACK_SIZE;
      uint64_t *sp = (uint64_t *)stack_base;
      printk(LOG_DEBUG, "  Kernel stack dump (bottom→top):\n");
      // Only dump the top 24 words (176 bytes trapframe + 56 bytes
      // switch_frame)
      int start = (KERNEL_STACK_SIZE / 8) - 24;
      for (int i = start; i < KERNEL_STACK_SIZE / 8; i++) {
        printk(LOG_DEBUG, "    [0x%016lX] 0x%016lX\n", stack_base + i * 8,
               sp[i]);
      }
      // Also dump k_rsp and the words around it
      printk(LOG_DEBUG, "  k_rsp=0x%016lX k_stack_top=0x%016lX\n",
             current_task->k_rsp, current_task->k_stack_top);
      // Dump 8 words starting at k_rsp
      printk(LOG_DEBUG, "  At k_rsp:\n");
      uint64_t *krsp_ptr = (uint64_t *)current_task->k_rsp;
      for (int i = 0; i < 16; i++) {
        printk(LOG_DEBUG, "    [0x%016lX] 0x%016lX\n",
               current_task->k_rsp + i * 8, krsp_ptr[i]);
      }
    }
  }

  if (tf->cs == 0x2B) {
    // User-mode: dump faulting instruction bytes so SSE/illegal-insn are
    // recognizable without offline objdump (e.g. f2 0f 10 = movsd).
    // User rip is mapped in the current CR3; safe to read from kernel.
    // Guard against NULL rip (e.g. jump to address 0) — reading from
    // address 0 in kernel context would cause a second page fault.
    {
      uint8_t *rip_ptr = (uint8_t *)tf->rip;
      printk(LOG_ERROR, "  user RIP bytes:");
      if (rip_ptr) {
        for (int i = 0; i < 16; i++) {
          printk(LOG_ERROR, " %02X", rip_ptr[i]);
        }
      } else {
        printk(LOG_ERROR, " (NULL)");
      }
      printk(LOG_ERROR, "\n");
    }

    // User-mode exception: translate to signal instead of killing process
    int sig = 0;
    int si_code = SI_KERNEL;
    void *si_addr = NULL;

    switch (tf->trapno) {
    case 0: // #DE divide error
      sig = SIGFPE;
      si_code = FPE_INTDIV;
      si_addr = NULL;
      break;
    case 6: // #UD illegal opcode
      sig = SIGILL;
      si_code = ILL_ILLOPC;
      si_addr = (void *)tf->rip;
      break;
    case 13: // #GP general protection
      sig = SIGSEGV;
      si_code = SEGV_MAPERR;
      si_addr = (void *)tf->rip;
      break;
    case 14: // #PF page fault — COW already resolved above; only genuine faults
             // reach here
    {
      uint64_t fault_addr = read_cr2();
      bool is_present = tf->err_code & 1;

      si_addr = (void *)fault_addr;

      if (!is_present) {
        // Not-present per hardware. A PROT_NONE page is present=0 but carries
        // PTE_PROTNONE: it is a valid (mprotect'd) mapping with no access, so
        // a fault on it is a protection violation (SEGV_ACCERR), not a missing
        // mapping (SEGV_MAPERR). Mirrors Linux's pte Protnone handling.
        sig = SIGSEGV;
        uint64_t *pte = lookup_pte(current_task->mm->cr3, fault_addr);
        if (pte && (*pte & PTE_PROTNONE))
          si_code = SEGV_ACCERR;
        else
          si_code = SEGV_MAPERR;
      } else {
        // Present but insufficient privilege: genuine protection violation
        sig = SIGSEGV;
        si_code = SEGV_ACCERR;
      }
    } break;
    default:
      sig = SIGSEGV;
      si_code = SI_KERNEL;
      si_addr = (void *)tf->rip;
      break;
    }

    printk(LOG_INFO, "exception: pid=%d vector=%lu sig=%d addr=%p\n",
           current_task->pid, (unsigned long)tf->trapno, sig, si_addr);

    if (force_sig_hook)
      force_sig_hook(current_task, sig, si_code, si_addr);
    // Check pending signals before returning to user mode
    if (signal_check_hook && current_task->proc) {
      signal_check_hook(current_task, tf);
    }
    return; // Don't kill process; check_pending_signals already handled it
  }
  // Kernel-mode exception: attempt exception-table fixup before panicking.
  // Only the user-copy instructions in copy_user.c are annotated; a fault on
  // any other kernel instruction is genuinely unrecoverable. vec 13 (#GP, e.g.
  // non-canonical address) and vec 14 (#PF) both use fixup semantics: #GP does
  // not report CR2 but the fixup (jump + -EFAULT) is identical. tf->cs==0x08
  // is the kernel-mode selector and gates this whole fallthrough (user-mode
  // tf->cs==0x2B is handled by the signal-translation branch above).
  if (tf->trapno == 13 || tf->trapno == 14) {
    uint64_t fixup = fixup_exception(tf);
    if (fixup) {
      tf->rip =
          fixup; // CPU resumes at the fixup label -> sets %rax=-EFAULT -> ret
      return;
    }
  }

  // Kernel-mode exception: unrecoverable, panic
  // (COW faults were already resolved above; genuine kernel #PF falls through.)
  panic("kernel-mode exception: vector=%lu rip=0x%lx cr3=0x%lx", tf->trapno,
        tf->rip, cr3);
}

// ===================== Timer IRQ handler =====================
// Reschedule IPI handler: set need_resched flag on current task and EOI.
// Per the reschedule IPI design (doc/design/kernel/schedule.md, c1 model):
// handler only sets flag, does NOT call schedule().  Preemption is uniformly
// handled by check_pending_signals at the return-to-user exit, which loops on
// need_resched -> schedule().
//
// bug.md Bug 2 root cause: previously this handler called schedule() directly
// when tf->cs==0x2B, which could re-enter schedule() inside IPI context.
// If the IPI interrupted an in-progress schedule() (already holding this core's
// scheduler_lock), the handler would re-enter schedule() on the same lock ->
// spin deadlock.  Symptoms: cpu0 timer keeps rising (timer IRQ doesn't need
// scheduler_lock) but sched count freezes, idle phase=4.
static void reschedule_ipi_handler(trapframe *tf) {
  (void)tf;
  current_task->need_resched = 1;
  lapic_eoi();
}

static void timer_handler(trapframe *tf) {
  tick++;
  lapic_eoi();

  if (tick % 100 == 0)
    printk(LOG_INFO, "timer alive: tick=%lu\n", tick);

  // Advance this CPU's RCU grace-period counter.  Without this, a CPU that
  // stays runnable in user mode (e.g. terminal flushing PTY output) never
  // enters sched_idle_entry and thus never calls rcu_read_unlock();
  // synchronize_rcu would spin forever waiting on cpu_gen[cpu].  IRQ context
  // (IF=0) is a quiescent state as long as we're not inside a read-side CS.
  rcu_quiescent();

  // Poll xHCI doorbell every ~10ms (every 10th tick at ~100Hz)
  // This ensures QEMU retries NAK'ed interrupt transfers
  if (tick % 10 == 0 && timer_poll_hook)
    timer_poll_hook();

  // Check timer queue for expired deadlines.
  // Two-phase approach: first, under local lock, detach expired tasks into
  // a temp list (touches only this CPU's timer_queue + task fields).  Then
  // deliver each to its target CPU's run_queue under that CPU's scheduler_lock.
  // run_queue/run_count must be protected by the owning CPU's scheduler_lock
  // — writing tcpu's run_queue cross-CPU while holding the local lock would
  // race with tcpu's schedule() on the same linked list/counter, causing
  // node loss, run_count drift, and eventually schedule()'s
  // ASSERT(cnt == run_count) panic.
  int cpu = get_cpu_local()->cpu_id;
  uint64_t now = sched_clock();
  list_node wakeup_list;
  list_init(&wakeup_list);

  uint64_t flags;
  spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags);
  list_node *head = &cpu_locals[cpu].timer_queue;
  while (!list_empty(head)) {
    xtask *p = LIST_ENTRY(list_front(head), xtask, wait_node);
    if (p->wait_deadline > now)
      break; // sorted, stop at first unexpired
    timer_queue_wait_pop(p);
    if (p->state == BLOCKED) {
      p->state = READY;
      p->wait_event = WAIT_NONE;
      p->wait_timed_out = 1;
      p->wait_deadline = 0;
      // pause() armed with an alarm: the timeout IS the alarm firing.
      // Force SIGALRM so the signal path (default-terminate or handler) runs
      // when the task resumes; the pending bit is set before run_queue push so
      // check_pending_signals sees it on resume.
      if (p->alarm_deadline != 0) {
        p->alarm_deadline = 0;
        if (force_sig_hook)
          force_sig_hook(p, SIGALRM, SI_KERNEL, NULL);
      }
      list_push_back(&wakeup_list, &p->wait_node);
    } else {
      // Stale entry: process was woken but sched_timer_queue_remove was skipped
      WARN_ON(1);
    }
  }
  spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock, flags);

  // Cross-CPU delivery: each task is queued under its assigned_cpu's lock.
  // Local CPU lock is already released.  Lock order is fixed as "single
  // target CPU lock", no nesting, no AB-BA risk.
  while (!list_empty(&wakeup_list)) {
    list_node *node = list_front(&wakeup_list);
    list_remove(node);
    xtask *p = LIST_ENTRY(node, xtask, wait_node);
    int tcpu = p->assigned_cpu;
    uint64_t tflags;
    spin_lock_irqsave(&cpu_locals[tcpu].scheduler_lock, &tflags);
    // design1 §4.5: timer delivery routes through run_queue_push so the
    // list_empty check unifies with wake_from_wait.  This is safe because
    // the state==BLOCKED gate (trap.c:605) + wait_node single-tenancy already
    // prevent a timer from seeing the same task twice; if a resource wake
    // already pushed run_node (wake_from_wait canceled the timer entry first),
    // list_empty is false and run_queue_push's ASSERT would trip — BUT that
    // cannot happen here because wake_from_wait's cancel removed this task's
    // wait_node from timer_queue, so timer_handler never reaches this task.
    // The mutual exclusion with wake_from_wait (same assigned_cpu lock) is
    // preserved: do NOT change the lock or drop it.
    run_queue_push(tcpu, p);
    spin_unlock_irqrestore(&cpu_locals[tcpu].scheduler_lock, tflags);
  }

  // timerfd expiry sweep: fire any armed timerfd whose deadline has passed.
  // Runs every tick (IRQ context, IF=0). Wakes blocked timerfd readers via
  // their file wait_queue. Registered by the BSD layer at init.
  if (timerfd_tick_hook)
    timerfd_tick_hook();

  // Running-task alarm: a process that armed an alarm and kept running (or is
  // blocked on something other than pause) is never in the timer_queue for its
  // alarm. Check the current task's alarm deadline each tick and force SIGALRM
  // when it fires; the user-mode signal check below then delivers it.
  if (current_task && current_task->alarm_deadline != 0 &&
      current_task->alarm_deadline <= now) {
    current_task->alarm_deadline = 0;
    if (force_sig_hook)
      force_sig_hook(current_task, SIGALRM, SI_KERNEL, NULL);
  }

  if (tf->cs == 0x2B) { // from user mode
    // Check pending signals before rescheduling
    if (signal_check_hook && current_task->proc) {
      signal_check_hook(current_task, tf);
    }
    // EXPERIMENT (NO_PREEMPT): skip setting need_resched so user-mode tasks
    // are never preempted mid-execution. If the libc.so .text corruption
    // disappears, the write happens during a context switch; if it remains,
    // pid=6 corrupts its own .text. Remove after the experiment.
#ifndef NO_PREEMPT
    current_task->need_resched =
        1; // set flag only, check_pending_signals calls schedule() at exit
#endif
  }

#ifndef NDEBUG
  // Preempt-stall watchdog (debug only): if need_resched is set but N
  // consecutive timer ticks pass without schedule() consuming it (schedule
  // clears need_resched on entry), a preemption point was bypassed.
  // Historically bug.md Bug 2 (ls hang) was caused by an unreachable
  // reschedule loop in check_pending_signals.  Print a warning pointing
  // at the preemption path to avoid misdiagnosing as work stealing /
  // load balance.  Zero cost in release builds.
  {
    int cpu = get_cpu_local()->cpu_id;
    if (current_task && current_task->need_resched) {
      if (++cpu_locals[cpu].preempt_stall_ticks >= 100) { // ~1s unconsumed
        printk(
            LOG_WARN,
            "PREEMPT-STALLED cpu%d pid%d need_resched unconsumed for %u ticks, "
            "this CPU run_queue ready=%d (preemption point bypassed? check "
            "check_pending_signals)\n",
            cpu, current_task->pid, cpu_locals[cpu].preempt_stall_ticks,
            cpu_locals[cpu].run_count);
      }
    } else {
      cpu_locals[cpu].preempt_stall_ticks = 0;
    }
  }
#endif
}

void irq_init() {
  // Initialize IRQ owner table
  for (int i = 0; i < MAX_IRQ_HANDLERS; i++) {
    irq_owner[i] = -1;
  }

  // Register default handlers
  irq_register(LAPIC_TIMER_VECTOR, timer_handler);
  irq_register(RESCHEDULE_VECTOR, reschedule_ipi_handler);

  // Re-initialize GDT with per-CPU setup (now running at virtual address)
  smp_init_cpu(0, 0, (int64_t)&stack_bottom + 8192);
  // Shared per-CPU bringup (GDT apply, NX/SSE, PAT, IDT, syscall MSRs, caps
  // log)
  cpu_bringup_common(0);
  // BSP-only: global APIC initialization (IOAPIC, interrupt routing, PIT mask)
  apic_init();

  // Verify BSP LAPIC timer is counting down
  {
    uint32_t ccr1 = lapic_read(LAPIC_TIMER_CCR);
    for (volatile int i = 0; i < 100000; i++)
      ; // brief delay
    uint32_t ccr2 = lapic_read(LAPIC_TIMER_CCR);
    printk(LOG_INFO, "irq_init: BSP timer CCR %u->%u LVT=0x%x (counting=%s)\n",
           ccr1, ccr2, lapic_read(LAPIC_LVT_TIMER),
           ccr1 != ccr2 ? "yes" : "NO!");
  }
}

// ===================== Syscall dispatch =====================
// Xcore IPC syscalls are dispatched directly via syscall_table.
// All other syscalls are delegated to syscall_dispatch.
#define NR_XCORE_SYSCALL (SYS_GETTID + 1)

static syscall_fn xcore_syscall_table[NR_XCORE_SYSCALL] = {
    [SYS_GETPID] = sys_getpid,     [SYS_YIELD] = sys_yield,
    [SYS_RECV] = sys_recv,         [SYS_REQ] = sys_req,
    [SYS_RESP] = sys_resp,         [SYS_IRQ_BIND] = sys_irq_bind,
    [SYS_NOTIFY] = sys_notify,     [SYS_GETTIME] = sys_gettime,
    [SYS_CLOCK] = sys_clock,       [SYS_MSG] = sys_msg,
    [SYS_MSG_RESP] = sys_msg_resp, [SYS_IOPERM] = sys_ioperm,
    [SYS_GETTID] = sys_gettid,
};

static const char *xcore_syscall_names[NR_XCORE_SYSCALL] = {
    [SYS_GETPID] = "getpid",     [SYS_YIELD] = "yield",
    [SYS_RECV] = "recv",         [SYS_REQ] = "req",
    [SYS_RESP] = "resp",         [SYS_IRQ_BIND] = "irq_bind",
    [SYS_NOTIFY] = "notify",     [SYS_GETTIME] = "gettime",
    [SYS_CLOCK] = "clock",       [SYS_MSG] = "msg",
    [SYS_MSG_RESP] = "msg_resp", [SYS_IOPERM] = "ioperm",
    [SYS_GETTID] = "gettid",
};

const char *syscall_name(uint64_t nr) {
  if (nr < NR_XCORE_SYSCALL && xcore_syscall_names[nr])
    return xcore_syscall_names[nr];
  // BSD syscalls have names in syscall_dispatch debug output
  return "bsd_syscall";
}

void xcall_dispatch(trapframe *tf) {
  get_cpu_local()->cur_tf = tf;
  int64_t nr = tf->rax;

  // Xcore IPC syscalls: dispatch directly
  if (nr >= 0 && nr < NR_XCORE_SYSCALL && xcore_syscall_table[nr]) {
    tf->rax = xcore_syscall_table[nr](tf->rdi, tf->rsi, tf->rdx, tf->r10,
                                      tf->r8, tf->r9);
  } else if (nr >= 0) {
    // All other syscalls → BSD layer
    int64_t ret =
        syscall_dispatch_hook ? syscall_dispatch_hook(tf) : (int64_t)-ENOSYS;
    // sys_sigreturn restores the full trapframe (including rax) from the
    // signal frame; its own return value must NOT overwrite tf->rax, or
    // the syscall return value saved at signal delivery (-EINTR etc.) is
    // clobbered back to 0.
    if (nr != SYS_SIGRETURN)
      tf->rax = ret;
  } else {
    printk(LOG_WARN, "xcall_dispatch: unknown syscall nr=%lu pid=%d\n",
           (unsigned long)tf->rax, current_task->pid);
    tf->rax = (int64_t)-ENOSYS;
  }

  // Check pending signals before returning to user mode
  if (signal_check_hook && current_task->proc && tf->cs == 0x2B) {
    signal_check_hook(current_task, tf);
  }
}
