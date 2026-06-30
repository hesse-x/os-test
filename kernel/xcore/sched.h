#ifndef KERNEL_XCORE_SCHED_H
#define KERNEL_XCORE_SCHED_H

#include <stdint.h>
#include "kernel/xcore/xtask.h"

// Scheduler and process table (kernel/xcore/sched.c)

xtask_t *xtask_alloc(void);
void xtask_free(xtask_t *t);
void proc_init(void);
void schedule(void);
void task_reap(xtask_t *proc);
xtask_t *create_idle_process(int cpu_id);
void idle_entry(void);
void timer_queue_insert(int cpu, xtask_t *proc);
void timer_queue_remove(xtask_t *proc);

static inline void timer_queue_cancel(xtask_t *proc) {
    if (proc->wait_deadline != 0) {
        timer_queue_remove(proc);
        proc->wait_deadline = 0;
    }
}

static inline void wake_from_wait(xtask_t *p) {
    timer_queue_cancel(p);
    p->state = READY;
    p->wait_event = WAIT_NONE;
    p->wait_timed_out = 0;
    int cpu = p->assigned_cpu;
    list_push_back(&cpu_locals[cpu].run_queue, &p->run_node);
    cpu_locals[cpu].run_count++;
}

// Assembly entry points
void switch_to(xtask_t *prev, xtask_t *next);
void process_entry(void);

// Internal helpers
int pick_cpu(void);
uint64_t build_kstack(uint64_t k_stack_top, uint64_t entry_rip);

#endif // KERNEL_XCORE_SCHED_H
