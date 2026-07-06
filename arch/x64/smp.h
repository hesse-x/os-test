/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARCH_X64_SMP_H
#define ARCH_X64_SMP_H

#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include <stdint.h>

#define MAX_CPUS 4
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_FS_BASE 0xC0000100
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_CSTAR 0xC0000083
#define MSR_SFMASK 0xC0000084

#define EFER_SCE (1ULL << 0)
#define EFER_NXE (1ULL << 11)

// RCU per-CPU nesting state (defined in kernel/rcu.h)
typedef struct rcu_local {
  int nesting;
  uint64_t saved_if;
} rcu_local;

typedef struct cpu_local {
  uint64_t saved_r10; // scratch slot: SYSCALL entry saves user R10 (arg4) here
  int cpu_id;
  uint32_t apic_id;
  struct xtask *_cur_proc;
  trapframe *cur_tf; // current trapframe (set by syscall/irq entry)
  void __iomem *lapic_base;
  uint64_t kernel_stack;
  uint64_t tss_rsp0;
  int run_count; // number of runnable processes on this CPU (excludes idle)
  struct xtask *idle_proc; // this CPU's idle process
  spinlock scheduler_lock; // per-CPU scheduler lock
  list_node run_queue;     // per-CPU ready queue (sentinel node)
  list_node timer_queue;   // per-CPU timer queue (sorted by wait_deadline,
                           // sentinel node)
  struct page
      *active_slab[NUM_KMALLOC_CLASSES]; // per-CPU active slab per size class

  // RCU read-side nesting state
  rcu_local rcu; // nesting count + saved IF

  // #NM nesting guard: detects fxrstor/fxsave executed with CR0.TS=1
  // (would nest #NM and blow the kernel stack into #DF). See fpu_lazy_switch.
  int nm_nesting_depth;

  // Preempt-stall watchdog (debug only): counts consecutive timer ticks
  // where need_resched is set but never consumed.  schedule() clears
  // need_resched on entry, resetting the counter.  If the counter exceeds
  // the threshold, a preemption point was bypassed (e.g. an unreachable
  // reschedule loop in check_pending_signals); we emit a warning and lock
  // the preemption path to prevent work stealing from misdiagnosing.
  // Release builds don't read or write this field — zero cost.
  uint32_t preempt_stall_ticks;
} cpu_local;

extern cpu_local cpu_locals[MAX_CPUS];
extern int ncpu;
extern gdt_entry per_cpu_gdt[MAX_CPUS][8];
extern gdt_ptr per_cpu_gdtr[MAX_CPUS];
extern struct tss_struct per_cpu_tss[MAX_CPUS];
extern uint64_t per_cpu_ist_stack[MAX_CPUS][3]; // IST1=NMI, IST2=DF, IST3=MCE

static inline void set_cpu_local(cpu_local *p) {
  // Kernel GS base holds cpu_local pointer; swapgs will exchange it to GS_BASE
  // when entering kernel from user mode
  wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)p);
}

static inline cpu_local *get_cpu_local() {
  return (cpu_local *)rdmsr(MSR_GS_BASE);
}

#define current_task (get_cpu_local()->_cur_proc)

void smp_init_cpu(int cpu_id, uint32_t apic_id, uint64_t kernel_stack);
void smp_apply_cpu(int cpu_id);

// Shared per-CPU bringup (GDT apply, NX/SSE, PAT, IDT, syscall MSRs, caps log).
// Called by irq_init (BSP, cpu_id=0) and ap_entry_c (APs) after smp_init_cpu.
// apic_init is NOT included: BSP does global APIC init, APs do per-CPU LAPIC
// enable.
void cpu_bringup_common(int cpu_id);

// AP startup
void ap_entry_c(int cpu_id);
void smp_boot_aps();

#endif // ARCH_X64_SMP_H
