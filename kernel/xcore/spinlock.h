#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include <stdint.h>
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/log.h"       // BUG_ON
#include "arch/x64/utils.h"   // rdmsr (debug builds only)

typedef struct spinlock_t {
    volatile uint32_t locked;
#ifndef NDEBUG
    int cpu_id;       // debug: track owner, recursive deadlock detection
#endif
} spinlock_t;

#ifdef NDEBUG
#define SPINLOCK_INIT (spinlock_t){.locked = 0}
#else
#define SPINLOCK_INIT (spinlock_t){.locked = 0, .cpu_id = -1}
#endif

#ifdef NDEBUG

/* Release: identical performance to original — no owner tracking */
static inline void spin_lock(spinlock_t *lk) __acquires(lk) {
    while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) == 1)
        __asm__ volatile("pause");
}

static inline void spin_unlock(spinlock_t *lk) __releases(lk) {
    __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);
}

#else

/* Debug: track owner + recursive deadlock detection
 * Uses rdmsr(MSR_GS_BASE) + 8 to read cpu_local_t.cpu_id
 * (offset 8: after saved_r10 uint64_t). rdmsr is a serializing
 * instruction (~30 cycles) — acceptable in debug builds only.
 * Cannot use get_cpu_local() here due to circular dependency
 * (smp.h -> spinlock.h -> smp.h). */
#define MSR_GS_BASE 0xC0000101

static inline int current_cpu_id_debug(void) {
    /* cpu_local_t layout: offset 0=saved_r10(uint64_t), offset 8=cpu_id(int) */
    return *(int *)(rdmsr(MSR_GS_BASE) + 8);
}

static inline void spin_lock(spinlock_t *lk) __acquires(lk) {
    BUG_ON(lk->locked && lk->cpu_id == current_cpu_id_debug());
    while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) == 1)
        __asm__ volatile("pause");
    lk->cpu_id = current_cpu_id_debug();
}

static inline void spin_unlock(spinlock_t *lk) __releases(lk) {
    lk->cpu_id = -1;
    __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);
}

#endif

static inline void spin_lock_irqsave(spinlock_t *lk, uint64_t *flags) __acquires(lk) __must_check {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f));
    *flags = f;
    spin_lock(lk);
}

static inline void spin_unlock_irqrestore(spinlock_t *lk, uint64_t flags) __releases(lk) {
    spin_unlock(lk);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags));
}

static inline int spin_trylock(spinlock_t *lk) __acquires(lk) {
    return __atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) == 0;
}

// trylock 失败时必须 popfq 恢复原 IF 状态,避免 cli 状态泄漏
static inline int spin_trylock_irqsave(spinlock_t *lk, uint64_t *flags) __acquires(lk) __must_check {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f));
    *flags = f;
    if (spin_trylock(lk)) return 1;
    __asm__ volatile("pushq %0; popfq" : : "r"(f));
    return 0;
}

#endif // KERNEL_SPINLOCK_H
