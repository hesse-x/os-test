#ifndef ARCH_X64_SMP_H
#define ARCH_X64_SMP_H

#include <stdint.h>
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "kernel/spinlock.h"
#include "kernel/list.h"
#include "kernel/mem/alloc.h"

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

struct task_t; // forward declaration (typedef in kernel/proc.h)

typedef struct cpu_local_t {
    uint64_t saved_r10;    // scratch slot: SYSCALL entry saves user R10 (arg4) here
    int cpu_id;
    uint32_t apic_id;
    struct task_t *_cur_proc;
    trapframe_t *cur_tf; // current trapframe (set by syscall/irq entry)
    void __iomem *lapic_base;
    uint64_t kernel_stack;
    uint64_t tss_rsp0;
    int run_count;         // number of runnable processes on this CPU (excludes idle)
    struct task_t *idle_proc;     // this CPU's idle process
    spinlock_t scheduler_lock; // per-CPU scheduler lock
    list_node_t run_queue;     // per-CPU ready queue (sentinel node)
    list_node_t timer_queue;   // per-CPU timer queue (sorted by wait_deadline, sentinel node)
    Page *active_slab[NUM_KMALLOC_CLASSES]; // per-CPU active slab per size class
} cpu_local_t;

extern cpu_local_t cpu_locals[MAX_CPUS];
extern int ncpu;
extern gdt_entry_t per_cpu_gdt[MAX_CPUS][8];
extern gdt_ptr_t per_cpu_gdtr[MAX_CPUS];
extern tss_t per_cpu_tss[MAX_CPUS];
extern uint64_t per_cpu_ist_stack[MAX_CPUS][3]; // IST1=NMI, IST2=DF, IST3=MCE

static inline void set_cpu_local(cpu_local_t *p) {
    // Kernel GS base holds cpu_local pointer; swapgs will exchange it to GS_BASE
    // when entering kernel from user mode
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)p);
}

static inline cpu_local_t *get_cpu_local() {
    return (cpu_local_t *)rdmsr(MSR_GS_BASE);
}

#define current_task (get_cpu_local()->_cur_proc)

void smp_init_cpu(int cpu_id, uint32_t apic_id, uint64_t kernel_stack);
void smp_apply_cpu(int cpu_id);

// AP startup
void ap_entry_c(int cpu_id);
void smp_boot_aps();

#endif // ARCH_X64_SMP_H
