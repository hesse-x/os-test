#ifndef KERNEL_RCU_H
#define KERNEL_RCU_H

#include <stdint.h>
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/spinlock.h"
#include "arch/x64/smp.h"     // MAX_CPUS, get_cpu_local

#define RCU_MAX_CPUS MAX_CPUS

typedef struct rcu_state {
    atomic_t global_gen;            // global generation counter, writer increments
    atomic_t cpu_gen[RCU_MAX_CPUS]; // per-CPU observed generation
    spinlock_t writer_lock;         // serialize writers (only one synchronize_rcu at a time)
} rcu_state_t;

extern rcu_state_t rcu_state;

// ===================== Per-CPU RCU nesting =====================
// rcu_local_t is defined in arch/x64/smp.h (embedded in cpu_local_t)

// Reader-side (nesting-safe, no flags parameter needed)
static inline void rcu_read_lock(void) {
    rcu_local_t *r = &get_cpu_local()->rcu;
    ASSERT(r->nesting < 8);  // catch runaway nesting (bug in unlock path)
    if (r->nesting++ == 0) {
        uint64_t flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));
        r->saved_if = flags;
    }
}

static inline void rcu_read_unlock(void) {
    rcu_local_t *r = &get_cpu_local()->rcu;
    ASSERT(r->nesting > 0);
    if (--r->nesting == 0) {
        int gen = atomic_read(&rcu_state.global_gen);
        int cpu = get_cpu_local()->cpu_id;
        atomic_set(&rcu_state.cpu_gen[cpu], gen);
        __asm__ volatile("pushq %0; popfq" : : "r"(r->saved_if));
    }
}

// Report a quiescent state from a CPU that is NOT in an RCU read-side
// critical section.  Used by timer_handler to advance grace periods on
// CPUs that stay runnable in user mode and would otherwise never pass
// through idle_entry's rcu_read_unlock().  If the CPU is inside a
// read-side CS (nesting > 0), this is a no-op — the in-flight
// rcu_read_unlock() will publish the quiescence when the CS ends.
//
// Caller context: IRQs disabled (e.g. timer IRQ).  No lock taken — only
// an atomic store to this CPU's cpu_gen slot.
static inline void rcu_quiescent(void) {
    rcu_local_t *r = &get_cpu_local()->rcu;
    if (r->nesting != 0) return;
    int gen = atomic_read(&rcu_state.global_gen);
    int cpu = get_cpu_local()->cpu_id;
    atomic_set(&rcu_state.cpu_gen[cpu], gen);
}

// Writer-side
void synchronize_rcu(void);  // wait for all CPUs to pass through a grace period
void rcu_init(void);

// RCU-protected pointer access
#define rcu_dereference(p) \
    ({ typeof(p) ___p = __atomic_load_n(&(p), __ATOMIC_CONSUME); ___p; })

#define rcu_assign_pointer(p, v) \
    do { __atomic_store_n(&(p), (v), __ATOMIC_RELEASE); } while(0)

#endif /* KERNEL_RCU_H */
